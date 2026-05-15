#include <Database/WriteAheadLog.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <fstream>
#include <mutex>
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

WriteAheadLog::WriteAheadLog(std::filesystem::path DbPath) {
	std::string S = DbPath.string();
	WalPath_ = std::filesystem::path(S + ".wal");
	BufferedLines_.reserve(kWalBatchLines);
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
	std::size_t Add = Line.size() + 1;
	if(!BufferedLines_.empty()) {
		std::size_t Pending = 0;
		for(const auto &L : BufferedLines_)
			Pending += L.size() + 1;
		if(Pending + Add >= kWalBatchBytes || BufferedLines_.size() >= kWalBatchLines)
			FlushBufferedUnlocked();
	}
	BufferedLines_.emplace_back(Line);
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
	std::string Line;
	while(std::getline(In, Line)) {
		if(!Line.empty() && Line.back() == '\r')
			Line.pop_back();
		if(Line.empty())
			continue;
		auto Tok = SplitPipe(Line);
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
				if(Rem != ColCount * 5 && Rem != ColCount * 7)
					FailWal("Corrupt WAL line: CREATE TABLE column field count mismatch (expected legacy "
					        "T| rows with 5 or extended with 7 fields per column): " +
					        WalPath_.string());
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
				Schema.push_back(std::move(Col));
			}
			Db.CreateTable(TableName, Schema).get();
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
