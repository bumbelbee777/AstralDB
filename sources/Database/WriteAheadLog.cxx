#include <Database/WriteAheadLog.hxx>
#include <Database/HybridStorageScheduler.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <DS/ErrorCorrection.hxx>
#include <DS/XChaCha20.hxx>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <vector>
#include <string_view>
#include <cstring>

namespace AstralDB {

namespace {
[[noreturn]] void FailWal(std::string Message) {
	throw std::runtime_error(Err::Prefixed("WAL", std::move(Message)));
}

/** Batch by record count and approximate byte volume for fewer syscalls. */
static constexpr std::size_t kWalBatchLines = 512;
static constexpr std::size_t kWalBatchBytes = 256 * 1024;

static const char *const kWalB64Enc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string WalEncodeBlob(std::string_view Plain) {
	std::string Out;
	Out.reserve(((Plain.size() + 2) / 3) * 4);
	uint32_t Acc = 0;
	int Bits = 0;
	for(unsigned char Ch : Plain) {
		Acc = (Acc << 8) | Ch;
		Bits += 8;
		while(Bits >= 6) {
			Bits -= 6;
			Out.push_back(kWalB64Enc[(Acc >> Bits) & 63]);
		}
	}
	if(Bits) {
		Acc <<= (6 - Bits);
		Out.push_back(kWalB64Enc[Acc & 63]);
	}
	while(Out.size() % 4)
		Out.push_back('=');
	return Out;
}

/** Base64url-safe alphabet for encoded CREATE VIEW bodies (no '|' in alphabet). */
static std::string WalDecodeSqlBody(std::string_view In) {
	std::string Out;
	Out.reserve(In.size() * 3 / 4);
	int Acc = 0;
	int Bits = -8;
	for(unsigned char C : In) {
		if(C == '=')
			break;
		int D = -1;
		if(C >= 'A' && C <= 'Z')
			D = static_cast<int>(C - 'A');
		else if(C >= 'a' && C <= 'z')
			D = static_cast<int>(C - 'a') + 26;
		else if(C >= '0' && C <= '9')
			D = static_cast<int>(C - '0') + 52;
		else if(C == '+')
			D = 62;
		else if(C == '/')
			D = 63;
		if(D < 0)
			continue;
		Acc = (Acc << 6) | D;
		Bits += 6;
		if(Bits >= 0) {
			Out.push_back(static_cast<char>((Acc >> Bits) & 255));
			Bits -= 8;
		}
	}
	return Out;
}
}

WriteAheadLog::WriteAheadLog(std::filesystem::path DbPath, const std::array<uint8_t, 32> &EncryptionKey)
	: WalKey_(EncryptionKey) {
	std::string S = DbPath.string();
	WalPath_ = std::filesystem::path(S + ".wal");
	BufferedLines_.reserve(kWalBatchLines);
}

std::string WriteAheadLog::EncryptRecordToLine(std::string_view PlainLine) const {
	const std::string Prot = DS::ErrorCorrection::Protect(PlainLine);
	std::array<uint8_t, 24> Nonce{};
	std::random_device Rd;
	for(auto &B : Nonce)
		B = static_cast<uint8_t>(Rd());
	std::vector<uint8_t> In(Prot.begin(), Prot.end()), Out;
	XChaCha20 Cipher(WalKey_, Nonce);
	Cipher.Encrypt(In, Out);
	std::string Blob;
	Blob.resize(24 + Out.size());
	std::memcpy(Blob.data(), Nonce.data(), 24);
	std::memcpy(Blob.data() + 24, Out.data(), Out.size());
	return std::string("W1|") + WalEncodeBlob(Blob);
}

void WriteAheadLog::FlushBufferedUnlocked() {
	if(BufferedLines_.empty())
		return;
	std::ofstream Out(WalPath_, std::ios::app | std::ios::binary);
	if(!Out)
		FailWal("Cannot append to write-ahead log (check disk space and permissions): " + WalPath_.string());
	std::size_t RunBytes = 0;
	for(const auto &Line : BufferedLines_) {
		Out.write(Line.data(), static_cast<std::streamsize>(Line.size()));
		Out.put('\n');
		RunBytes += Line.size() + 1;
		/** Periodic flush lets readers see progress without one huge kernel buffer. */
		if(RunBytes >= kWalBatchBytes) {
			Out.flush();
			RunBytes = 0;
		}
	}
	BufferedLines_.clear();
}

void WriteAheadLog::AppendLine(std::string_view Line) {
	std::lock_guard<AstralDB::Mutex> Lk(Mut_);
	const std::string Enc = EncryptRecordToLine(Line);
	std::size_t Add = Enc.size() + 1;
	if(!BufferedLines_.empty()) {
		std::size_t Pending = 0;
		for(const auto &L : BufferedLines_)
			Pending += L.size() + 1;
		if(Pending + Add >= kWalBatchBytes || BufferedLines_.size() >= kWalBatchLines)
			FlushBufferedUnlocked();
	}
	BufferedLines_.emplace_back(std::move(Enc));
	if(BufferedLines_.size() >= kWalBatchLines)
		FlushBufferedUnlocked();
}

void WriteAheadLog::Flush() {
	std::lock_guard<AstralDB::Mutex> Lk(Mut_);
	FlushBufferedUnlocked();
}

void WriteAheadLog::Truncate() {
	std::lock_guard<AstralDB::Mutex> Lk(Mut_);
	FlushBufferedUnlocked();
	if(Exists())
		std::filesystem::resize_file(WalPath_, 0);
}

static std::vector<std::string> SplitPipe(std::string_view Line) {
	std::vector<std::string> Out;
	size_t Start = 0;
	for(size_t i = 0; i < Line.size(); ++i) {
		if(Line[i] == '|') {
			Out.emplace_back(Line.substr(Start, i - Start));
			Start = i + 1;
		}
	}
	Out.emplace_back(Line.substr(Start));
	return Out;
}

void WriteAheadLog::Replay(Database &Db) {
	Flush();
	if(!Exists())
		return;
	std::ifstream In(WalPath_, std::ios::binary);
	if(!In)
		FailWal("Cannot read write-ahead log for replay: " + WalPath_.string());
	std::string WalLine;
	while(std::getline(In, WalLine)) {
		if(!WalLine.empty() && WalLine.back() == '\r')
			WalLine.pop_back();
		if(WalLine.empty())
			continue;
		if(WalLine.size() >= 3 && WalLine[0] == 'W' && WalLine[1] == '1' && WalLine[2] == '|') {
			const std::string Blob = WalDecodeSqlBody(std::string_view(WalLine).substr(3));
			if(Blob.size() < 24)
				FailWal("Encrypted WAL record too short - delete or repair " + WalPath_.string());
			std::array<uint8_t, 24> Nonce{};
			std::memcpy(Nonce.data(), Blob.data(), 24);
			std::vector<uint8_t> Ct(Blob.begin() + 24, Blob.end()), Pt;
			XChaCha20 Cipher(WalKey_, Nonce);
			Cipher.Decrypt(Ct, Pt);
			const std::string Fe(reinterpret_cast<const char *>(Pt.data()), Pt.size());
			const auto Rec = DS::ErrorCorrection::Recover(Fe);
			if(!Rec.has_value())
				FailWal("WAL FEC could not correct payload - delete or repair " + WalPath_.string());
			WalLine = *Rec;
		}
		auto Tok = SplitPipe(WalLine);
		if(Tok.empty())
			continue;
		if(Tok[0] == "T") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: CREATE TABLE (T) record has too few fields - delete or repair " +
				        WalPath_.string());
			const std::string &TableName = Tok[1];
			size_t ColCount = static_cast<size_t>(std::stoull(Tok[2]));
			Database::Schema Schema;
			size_t Idx = 3;
			const size_t Rem = Tok.size() > Idx ? Tok.size() - Idx : 0;
			size_t Wide = 0;
			if(ColCount == 0 && Rem != 0)
				FailWal("Corrupt WAL line: CREATE TABLE declares zero columns - delete or repair " +
				        WalPath_.string());
			if(ColCount > 0) {
				if(Rem != ColCount * 5 && Rem != ColCount * 7 && Rem != ColCount * 9)
					FailWal("Corrupt WAL line: CREATE TABLE column field count mismatch (expected legacy "
					        "T| rows with 5, 7, or 9 fields per column): " +
					        WalPath_.string());
				if(Rem == ColCount * 9)
					Wide = 9;
				else
					Wide = (Rem == ColCount * 7) ? 7 : 5;
			}
			for(size_t c = 0; c < ColCount; ++c) {
				if(Idx + Wide > Tok.size())
					FailWal("Corrupt WAL line: truncated CREATE TABLE column data - delete or repair " +
					        WalPath_.string());
				Database::Column Col;
				Col.Name = Tok[Idx++];
				Col.DefaultValue = Tok[Idx++];
				Col.IsPrimaryKey = Tok[Idx++] == "1";
				Col.IsUnique = Tok[Idx++] == "1";
				Col.IsNotNull = Tok[Idx++] == "1";
				if(Wide >= 7) {
					const std::string &Sq = Tok[Idx++];
					const std::string &Df = Tok[Idx++];
					if(!Sq.empty() && Sq != "-")
						Col.CheckConstraintSql = WalDecodeSqlBody(Sq);
					if(!Df.empty() && Df != "-")
						Col.CheckConstraintDnfPacked = WalDecodeSqlBody(Df);
				}
				if(Wide >= 9) {
					const std::string &IdA = Tok[Idx++];
					const std::string &IdS = Tok[Idx++];
					if(IdA == "1" || IdA == "0") {
						Col.IsIdentity = true;
						Col.IdentityAlways = IdA == "1";
						if(!IdS.empty() && IdS != "-")
							Col.IdentitySequenceName = IdS;
					}
				}
				Schema.push_back(std::move(Col));
			}
			Db.CreateTable(TableName, Schema).get();
		} else if(Tok[0] == "ST") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: SET STORAGE (ST) record incomplete - delete or repair " +
				        WalPath_.string());
			const std::string TableName = WalDecodeSqlBody(Tok[1]);
			const int Pol = std::stoi(Tok[2]);
			Db.ReplayWalSetTableStorage(TableName, static_cast<StorageLayout>(Pol));
		} else if(Tok[0] == "I") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: INSERT (I) record has too few fields - delete or repair " +
				        WalPath_.string());
			const std::string &TableName = Tok[1];
			size_t N = static_cast<size_t>(std::stoull(Tok[2]));
			if(Tok.size() < 3 + 2 * N)
				FailWal("Corrupt WAL line: truncated INSERT row payload - delete or repair " + WalPath_.string());
			Database::Item Row;
			size_t Base = 3;
			for(size_t i = 0; i < N; ++i)
				Row[Tok[Base + i * 2]] = Tok[Base + i * 2 + 1];
			Db.Insert(TableName, Row).get();
		} else if(Tok[0] == "D") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: DROP TABLE (D) record incomplete - delete or repair " + WalPath_.string());
			Db.DropTable(Tok[1]).get();
		} else if(Tok[0] == "AC") {
			if(Tok.size() < 7)
				FailWal("Corrupt WAL line: ALTER ADD COLUMN (AC) record incomplete - delete or repair " +
				        WalPath_.string());
			Database::Column C;
			C.Name = Tok[2];
			C.DefaultValue = Tok[3];
			C.IsPrimaryKey = Tok[4] == "1";
			C.IsUnique = Tok[5] == "1";
			C.IsNotNull = Tok[6] == "1";
			Db.AddColumn(Tok[1], C).get();
		} else if(Tok[0] == "DC") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: ALTER DROP COLUMN (DC) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.DropColumn(Tok[1], Tok[2]).get();
		} else if(Tok[0] == "RC") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: ALTER RENAME COLUMN (RC) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.RenameColumn(Tok[1], Tok[2], Tok[3]).get();
		} else if(Tok[0] == "V") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: CREATE VIEW (V) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.ReplayWalDefineView(Tok[1], WalDecodeSqlBody(Tok[2]));
		} else if(Tok[0] == "DV") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: DROP VIEW (DV) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.ReplayWalDropView(Tok[1]);
		} else if(Tok[0] == "PR") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: CREATE PROCEDURE (PR) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.ReplayWalDefineProcedure(Tok[1], WalDecodeSqlBody(Tok[2]));
		} else if(Tok[0] == "PD") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: DROP PROCEDURE (PD) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.ReplayWalDropProcedure(Tok[1]);
		} else if(Tok[0] == "UU") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: ADD USER (UU) record incomplete - delete or repair " + WalPath_.string());
			const std::string Name = WalDecodeSqlBody(Tok[1]);
			const std::string Enc = WalDecodeSqlBody(Tok[2]);
			const std::string KeyStr = WalDecodeSqlBody(Tok[3]);
			if(KeyStr.size() != 32)
				FailWal("Corrupt WAL line: ADD USER key length - delete or repair " + WalPath_.string());
			std::array<uint8_t, 32> K{};
			std::memcpy(K.data(), KeyStr.data(), 32);
			Db.ReplayWalAddUser(std::move(Name), std::move(Enc), K);
		} else if(Tok[0] == "UD") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: DROP USER (UD) record incomplete - delete or repair " + WalPath_.string());
			Db.ReplayWalRemoveUser(WalDecodeSqlBody(Tok[1]));
		} else if(Tok[0] == "UG") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: GRANT ACL (UG) record incomplete - delete or repair " + WalPath_.string());
			const int Bits = std::stoi(Tok[3]);
			Db.ReplayWalGrantAcl(WalDecodeSqlBody(Tok[1]), WalDecodeSqlBody(Tok[2]), Bits);
		} else if(Tok[0] == "UR") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: REVOKE ACL (UR) record incomplete - delete or repair " + WalPath_.string());
			const int Bits = std::stoi(Tok[3]);
			Db.ReplayWalRevokeAcl(WalDecodeSqlBody(Tok[1]), WalDecodeSqlBody(Tok[2]), Bits);
		} else if(Tok[0] == "SQ") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: CREATE SEQUENCE (SQ) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.ReplayWalCreateSequence(WalDecodeSqlBody(Tok[1]), std::stoll(Tok[2]), std::stoll(Tok[3]));
		} else if(Tok[0] == "SD") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: DROP SEQUENCE (SD) record incomplete - delete or repair " + WalPath_.string());
			Db.ReplayWalDropSequence(WalDecodeSqlBody(Tok[1]));
		} else if(Tok[0] == "CR") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: CREATE ROLE (CR) record incomplete - delete or repair " + WalPath_.string());
			Db.ReplayWalCreateRole(WalDecodeSqlBody(Tok[1]));
		} else if(Tok[0] == "DR") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: DROP ROLE (DR) record incomplete - delete or repair " + WalPath_.string());
			Db.ReplayWalDropRole(WalDecodeSqlBody(Tok[1]));
		} else if(Tok[0] == "GM") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: GRANT ROLE MEMBERSHIP (GM) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.ReplayWalGrantRoleMembership(WalDecodeSqlBody(Tok[1]), WalDecodeSqlBody(Tok[2]));
		} else if(Tok[0] == "RM") {
			if(Tok.size() < 3)
				FailWal("Corrupt WAL line: REVOKE ROLE MEMBERSHIP (RM) record incomplete - delete or repair " +
				        WalPath_.string());
			Db.ReplayWalRevokeRoleMembership(WalDecodeSqlBody(Tok[1]), WalDecodeSqlBody(Tok[2]));
		} else if(Tok[0] == "RG") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: GRANT ROLE ACL (RG) record incomplete - delete or repair " + WalPath_.string());
			const int Bits = std::stoi(Tok[3]);
			Db.ReplayWalGrantRoleAcl(WalDecodeSqlBody(Tok[1]), WalDecodeSqlBody(Tok[2]), Bits);
		} else if(Tok[0] == "RR") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: REVOKE ROLE ACL (RR) record incomplete - delete or repair " + WalPath_.string());
			const int Bits = std::stoi(Tok[3]);
			Db.ReplayWalRevokeRoleAcl(WalDecodeSqlBody(Tok[1]), WalDecodeSqlBody(Tok[2]), Bits);
		} else if(Tok[0] == "UF") {
			if(Tok.size() < 6)
				FailWal("Corrupt WAL line: FINE GRANT (UF) record incomplete - delete or repair " + WalPath_.string());
			RowColPermission Rule;
			Rule.Table = WalDecodeSqlBody(Tok[2]);
			Rule.RowId = WalDecodeSqlBody(Tok[3]);
			Rule.Column = WalDecodeSqlBody(Tok[4]);
			Rule.Perms = static_cast<Permissions>(std::stoi(Tok[5]));
			Db.ReplayWalFineGrant(WalDecodeSqlBody(Tok[1]), std::move(Rule));
		} else if(Tok[0] == "XF") {
			if(Tok.size() < 6)
				FailWal("Corrupt WAL line: FINE REVOKE (XF) record incomplete - delete or repair " + WalPath_.string());
			RowColPermission Rule;
			Rule.Table = WalDecodeSqlBody(Tok[2]);
			Rule.RowId = WalDecodeSqlBody(Tok[3]);
			Rule.Column = WalDecodeSqlBody(Tok[4]);
			Rule.Perms = static_cast<Permissions>(std::stoi(Tok[5]));
			Db.ReplayWalFineRevoke(WalDecodeSqlBody(Tok[1]), std::move(Rule));
		} else if(Tok[0] == "F") {
			if(Tok.size() < 6)
				FailWal("Corrupt WAL line: FOREIGN KEY (F) record incomplete - delete or repair " + WalPath_.string());
			ForeignKey Fk;
			const std::string Tab = WalDecodeSqlBody(Tok[1]);
			Fk.ColumnName = WalDecodeSqlBody(Tok[2]);
			Fk.ReferencedTable = WalDecodeSqlBody(Tok[3]);
			Fk.ReferencedColumn = WalDecodeSqlBody(Tok[4]);
			const int Act = std::stoi(Tok[5]);
			if(Act < 0 || Act > 2)
				FailWal("Corrupt WAL line: FOREIGN KEY action - delete or repair " + WalPath_.string());
			Fk.OnDelete = static_cast<ReferentialAction>(static_cast<uint8_t>(Act));
			Db.ReplayWalAddForeignKey(Tab, std::move(Fk));
		} else if(Tok[0] == "GR") {
			if(Tok.size() < 12)
				FailWal("Corrupt WAL line: GRAPH REGISTER (GR) record incomplete - delete or repair " +
				        WalPath_.string());
			GraphSpec Spec;
			Spec.Name = Tok[1];
			Spec.VertexTable = Tok[2];
			Spec.VertexIdCol = Tok[3];
			Spec.EdgeTable = Tok[4];
			Spec.EdgeSrcCol = Tok[5];
			Spec.EdgeDstCol = Tok[6];
			Spec.EdgeLabelCol = Tok[7];
			Spec.EdgeWeightCol = Tok[8];
			Spec.Undirected = Tok[9] == "1";
			Spec.ProjectionOf = Tok[10];
			Spec.ProjectionEdgeFilter = Tok[11];
			Db.ReplayWalGraphRegister(std::move(Spec));
		} else if(Tok[0] == "GD") {
			if(Tok.size() < 2)
				FailWal("Corrupt WAL line: GRAPH DROP (GD) record incomplete - delete or repair " + WalPath_.string());
			Db.ReplayWalGraphDrop(Tok[1]);
		} else if(Tok[0] == "GP") {
			if(Tok.size() < 4)
				FailWal("Corrupt WAL line: GRAPH PROJECTION (GP) record incomplete - delete or repair " +
				        WalPath_.string());
			GraphProjectionRequest Req;
			Req.ProjectionName = Tok[1];
			Req.BaseGraphName = Tok[2];
			Req.EdgeLabelFilter = Tok[3];
			Db.ReplayWalGraphProjection(Req);
		}
	}
}

WriteAheadLog::~WriteAheadLog() {
	try {
		std::lock_guard<AstralDB::Mutex> Lk(Mut_);
		FlushBufferedUnlocked();
	} catch(...) {
	}
}

}
