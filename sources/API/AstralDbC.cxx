#include <astraldb/AstralDB.h>

#include <IO/Logger.hxx>
#include <SQL/SQL.hxx>
#include <SQL/BytecodeInterpreter.hxx>

#include <cctype>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using AstralDB::Database;

struct DbHandle {
	std::unique_ptr<AstralDB::Logger> Logger_;
	AstralDB::SQL::BytecodeInterpreter Interp_;
	std::string LastError_;

	explicit DbHandle(std::filesystem::path Path)
	    : Logger_(std::make_unique<AstralDB::Logger>("AstralDbC_api.log", false)), Interp_(Logger_.get()) {
		Interp_.DatabasePath(std::move(Path));
	}
};

struct StmtHandle {
	DbHandle *Db_ = nullptr;
	std::string SqlTemplate_;
	std::vector<std::string> Binds_;
	std::vector<Database::Item> Rows_;
	std::vector<std::string> ColumnOrder_;
	std::size_t Cursor_ = 0;
	std::string LastValue_;
	bool Loaded_ = false;
};

std::string QuoteSqlLiteral(std::string_view Raw) {
	std::string Out = "'";
	for(char Ch : Raw) {
		if(Ch == '\'')
			Out += "''";
		else
			Out += Ch;
	}
	Out += '\'';
	return Out;
}

static bool IsIntegerLiteral(std::string_view S) {
	if(S.empty())
		return false;
	std::size_t I = 0;
	if(S[0] == '-' && S.size() > 1)
		++I;
	if(I >= S.size())
		return false;
	for(; I < S.size(); ++I) {
		if(!std::isdigit(static_cast<unsigned char>(S[I])))
			return false;
	}
	return true;
}

std::string ApplyStmtBinds(const std::string &Sql, const std::vector<std::string> &Binds) {
	std::string Out;
	Out.reserve(Sql.size());
	int NextBind = 1;
	for(std::size_t I = 0; I < Sql.size(); ++I) {
		if(Sql[I] == '?') {
			if(NextBind < 1 || static_cast<std::size_t>(NextBind) > Binds.size())
				throw std::runtime_error("AstralDb: unbound ? placeholder in prepared statement.");
			const std::string &Bound = Binds[static_cast<std::size_t>(NextBind - 1)];
			if(IsIntegerLiteral(Bound))
				Out += Bound;
			else
				Out += QuoteSqlLiteral(Bound);
			++NextBind;
		} else {
			Out += Sql[I];
		}
	}
	return Out;
}

int CountBindPlaceholders(const std::string &Sql) {
	int InString = 0;
	int Count = 0;
	for(std::size_t I = 0; I < Sql.size(); ++I) {
		const char C = Sql[I];
		if(InString) {
			if(C == InString && (I == 0 || Sql[I - 1] != '\\'))
				InString = 0;
			continue;
		}
		if(C == '\'' || C == '"') {
			InString = C;
			continue;
		}
		if(C == '?')
			++Count;
	}
	return Count;
}

std::string ExtractFromTableName(const std::string &Sql) {
	std::string Upper = Sql;
	for(char &Ch : Upper)
		Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
	const std::string Needle = " FROM ";
	const size_t Pos = Upper.find(Needle);
	if(Pos == std::string::npos)
		return {};
	size_t Start = Pos + Needle.size();
	while(Start < Sql.size() && std::isspace(static_cast<unsigned char>(Sql[Start])))
		++Start;
	size_t End = Start;
	while(End < Sql.size() && (std::isalnum(static_cast<unsigned char>(Sql[End])) || Sql[End] == '_'))
		++End;
	if(End <= Start)
		return {};
	return Sql.substr(Start, End - Start);
}

bool ExecuteSql(DbHandle &Db, const std::string &Sql) {
	try {
		std::set<std::string> TablesBefore;
		Db.Interp_.EnsurePrimaryDatabaseOpened();
		Database *Primary = Db.Interp_.PrimaryDatabase();
		if(Primary != nullptr) {
			Primary->WithExclusiveBytecodeLock([&]() {
				for(const auto &Pr : Primary->Tables_)
					TablesBefore.insert(Pr.first);
			});
		}

		AstralDB::SQL::Parser Parser(Sql);
		AstralDB::SQL::Bytecode Code =
		    AstralDB::SQL::BuildBytecode(Db.Logger_.get(), AstralDB::SQL::OptimizationLevel::None);
		Db.Interp_.Execute(Code);
		Db.LastError_.clear();
		return true;
	} catch(const std::exception &Ex) {
		Db.LastError_ = Ex.what();
		return false;
	}
}

void CaptureSelectRows(StmtHandle &Stmt, const std::string &Sql) {
	Stmt.Db_->Interp_.EnsurePrimaryDatabaseOpened();
	Database *Primary = Stmt.Db_->Interp_.PrimaryDatabase();
	if(Primary == nullptr)
		return;
	if(!ExecuteSql(*Stmt.Db_, Sql))
		return;
	const std::string OutTable = ExtractFromTableName(Sql);
	if(OutTable.empty()) {
		Stmt.Db_->LastError_ = "AstralDbPrepare could not resolve SELECT source table.";
		return;
	}
	auto Rows = Primary->Select(OutTable, [](const Database::Item &) { return true; }).get();
	Stmt.Rows_ = std::move(Rows);
	auto Schema = Primary->TableSchemaSnapshot(OutTable);
	if(Schema.has_value()) {
		for(const auto &Col : *Schema)
			Stmt.ColumnOrder_.push_back(Col.Name);
	}
}

} // namespace

struct AstralDb {
	DbHandle Impl;
	explicit AstralDb(std::filesystem::path Path) : Impl(std::move(Path)) {}
};

struct AstralDbStmt {
	StmtHandle Impl;
};

extern "C" {

const char *AstralDbVersion(void) {
	return ASTRALDB_VERSION;
}

int AstralDbOpen(const char *Path, AstralDb **OutDb) {
	if(Path == nullptr || OutDb == nullptr)
		return -1;
	try {
		*OutDb = nullptr;
		*OutDb = new AstralDb(Path);
		return 0;
	} catch(...) {
		*OutDb = nullptr;
		return -1;
	}
}

void AstralDbClose(AstralDb *Db) {
	delete Db;
}

int AstralDbExec(AstralDb *Db, const char *Sql) {
	if(Db == nullptr || Sql == nullptr)
		return -1;
	return ExecuteSql(Db->Impl, Sql) ? 0 : -1;
}

int AstralDbCall(AstralDb *Db, const char *ProcedureName) {
	if(Db == nullptr || ProcedureName == nullptr || *ProcedureName == '\0')
		return -1;
	std::string Sql = "CALL ";
	Sql += ProcedureName;
	Sql += ';';
	return AstralDbExec(Db, Sql.c_str());
}

const char *AstralDbLastError(const AstralDb *Db) {
	if(Db == nullptr)
		return "AstralDb handle is null";
	return Db->Impl.LastError_.c_str();
}

static int ValidateSelectTemplate(DbHandle &Db, const std::string &Query, StmtHandle &Impl) {
	std::size_t Pos = 0;
	while(Pos < Query.size() && std::isspace(static_cast<unsigned char>(Query[Pos])))
		++Pos;
	std::string Verb;
	for(; Pos < Query.size() && std::isalpha(static_cast<unsigned char>(Query[Pos])); ++Pos)
		Verb.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(Query[Pos]))));
	if(Verb != "SELECT") {
		Db.LastError_ = "AstralDbPrepare currently supports SELECT statements only.";
		return -1;
	}
	const int Need = CountBindPlaceholders(Query);
	if(Need > 0)
		Impl.Binds_.resize(static_cast<std::size_t>(Need));
	return 0;
}

static int LoadSelectStmt(StmtHandle &Impl) {
	if(Impl.Db_ == nullptr || Impl.SqlTemplate_.empty())
		return -1;
	const int Need = CountBindPlaceholders(Impl.SqlTemplate_);
	if(Need > 0) {
		if(static_cast<int>(Impl.Binds_.size()) < Need)
			Impl.Binds_.resize(static_cast<std::size_t>(Need));
		for(int I = 0; I < Need; ++I) {
			if(Impl.Binds_[static_cast<std::size_t>(I)].empty()) {
				Impl.Db_->LastError_ = "AstralDbStmt: bind all ? placeholders before stepping.";
				return -1;
			}
		}
	}
	const std::string Resolved =
	    Need > 0 ? ApplyStmtBinds(Impl.SqlTemplate_, Impl.Binds_) : Impl.SqlTemplate_;
	Impl.Rows_.clear();
	Impl.ColumnOrder_.clear();
	Impl.Cursor_ = 0;
	CaptureSelectRows(Impl, Resolved);
	if(!Impl.Db_->LastError_.empty())
		return -1;
	Impl.Loaded_ = true;
	return 0;
}

int AstralDbExecQuery(AstralDb *Db, const char *Sql, AstralDbRowCallback Callback, void *Ctx) {
	if(Db == nullptr || Sql == nullptr)
		return -1;
	if(Callback == nullptr)
		return AstralDbExec(Db, Sql);
	try {
		StmtHandle Tmp;
		Tmp.Db_ = &Db->Impl;
		Tmp.SqlTemplate_ = Sql;
		if(ValidateSelectTemplate(Db->Impl, Sql, Tmp) != 0)
			return -1;
		if(LoadSelectStmt(Tmp) != 0)
			return -1;
		int Row = 0;
		for(const auto &Item : Tmp.Rows_) {
			int Col = 0;
			for(const auto &Name : Tmp.ColumnOrder_) {
				const char *Text = nullptr;
				if(const auto It = Item.find(Name); It != Item.end())
					Text = It->second.c_str();
				if(Callback(Ctx, Row, Col, Name.c_str(), Text) != 0)
					return 0;
				++Col;
			}
			++Row;
		}
		Db->Impl.LastError_.clear();
		return 0;
	} catch(const std::exception &Ex) {
		Db->Impl.LastError_ = Ex.what();
		return -1;
	}
}

int AstralDbPrepare(AstralDb *Db, const char *Sql, AstralDbStmt **OutStmt) {
	if(Db == nullptr || Sql == nullptr || OutStmt == nullptr)
		return -1;
	try {
		*OutStmt = nullptr;
		auto *Stmt = new AstralDbStmt();
		Stmt->Impl.Db_ = &Db->Impl;
		Stmt->Impl.SqlTemplate_ = Sql;
		if(ValidateSelectTemplate(Db->Impl, Stmt->Impl.SqlTemplate_, Stmt->Impl) != 0) {
			delete Stmt;
			return -1;
		}
		Db->Impl.LastError_.clear();
		*OutStmt = Stmt;
		return 0;
	} catch(const std::exception &Ex) {
		Db->Impl.LastError_ = Ex.what();
		*OutStmt = nullptr;
		return -1;
	}
}

int AstralDbBindInt64(AstralDbStmt *Stmt, int Index, long long Value) {
	if(Stmt == nullptr || Index < 1)
		return -1;
	try {
		if(static_cast<std::size_t>(Index) > Stmt->Impl.Binds_.size())
			Stmt->Impl.Binds_.resize(static_cast<std::size_t>(Index));
		Stmt->Impl.Binds_[static_cast<std::size_t>(Index - 1)] = std::to_string(Value);
		return 0;
	} catch(...) {
		return -1;
	}
}

int AstralDbBindText(AstralDbStmt *Stmt, int Index, const char *Value) {
	if(Stmt == nullptr || Index < 1)
		return -1;
	try {
		if(static_cast<std::size_t>(Index) > Stmt->Impl.Binds_.size())
			Stmt->Impl.Binds_.resize(static_cast<std::size_t>(Index));
		Stmt->Impl.Binds_[static_cast<std::size_t>(Index - 1)] = Value == nullptr ? std::string() : std::string(Value);
		return 0;
	} catch(...) {
		return -1;
	}
}

int AstralDbStmtStep(AstralDbStmt *Stmt) {
	if(Stmt == nullptr)
		return -1;
	if(!Stmt->Impl.Loaded_) {
		if(LoadSelectStmt(Stmt->Impl) != 0)
			return -1;
	}
	if(Stmt->Impl.Cursor_ >= Stmt->Impl.Rows_.size())
		return 0;
	++Stmt->Impl.Cursor_;
	return 1;
}

void AstralDbStmtReset(AstralDbStmt *Stmt) {
	if(Stmt == nullptr)
		return;
	Stmt->Impl.Cursor_ = 0;
	Stmt->Impl.LastValue_.clear();
	Stmt->Impl.Rows_.clear();
	Stmt->Impl.ColumnOrder_.clear();
	Stmt->Impl.Loaded_ = false;
}

int AstralDbStmtColumnCount(const AstralDbStmt *Stmt) {
	if(Stmt == nullptr)
		return -1;
	return static_cast<int>(Stmt->Impl.ColumnOrder_.size());
}

const char *AstralDbStmtColumnName(const AstralDbStmt *Stmt, int Col) {
	if(Stmt == nullptr || Col < 0 || static_cast<std::size_t>(Col) >= Stmt->Impl.ColumnOrder_.size())
		return nullptr;
	return Stmt->Impl.ColumnOrder_[static_cast<std::size_t>(Col)].c_str();
}

const char *AstralDbStmtColumnText(const AstralDbStmt *StmtIn, int Col) {
	if(StmtIn == nullptr || Col < 0)
		return nullptr;
	auto *Stmt = const_cast<AstralDbStmt *>(StmtIn);
	if(Stmt->Impl.Cursor_ == 0 || Stmt->Impl.Cursor_ > Stmt->Impl.Rows_.size())
		return nullptr;
	const auto &Row = Stmt->Impl.Rows_[Stmt->Impl.Cursor_ - 1];
	const std::size_t Idx = static_cast<std::size_t>(Col);
	if(Idx >= Stmt->Impl.ColumnOrder_.size())
		return nullptr;
	auto It = Row.find(Stmt->Impl.ColumnOrder_[Idx]);
	if(It == Row.end())
		return nullptr;
	Stmt->Impl.LastValue_ = It->second;
	return Stmt->Impl.LastValue_.c_str();
}

void AstralDbStmtFinalize(AstralDbStmt *Stmt) {
	delete Stmt;
}

} // extern "C"
