#include <astraldb/astraldb.h>

#include <IO/Logger.hxx>
#include <SQL/SQL.hxx>
#include <SQL/BytecodeInterpreter.hxx>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using AstralDB::Database;

struct DbHandle {
	std::unique_ptr<AstralDB::Logger> Logger;
	AstralDB::SQL::BytecodeInterpreter Interp;
	std::string LastError;

	explicit DbHandle(std::filesystem::path Path)
	    : Logger(std::make_unique<AstralDB::Logger>("astraldb_c_api.log", false)), Interp(Logger.get()) {
		Interp.DatabasePath(std::move(Path));
	}
};

struct StmtHandle {
	DbHandle *Db = nullptr;
	std::vector<Database::Item> Rows;
	std::vector<std::string> ColumnOrder;
	std::size_t Cursor = 0;
	std::string LastValue;
};

static std::string ExtractFromTableName(const std::string &Sql) {
	std::string Upper = Sql;
	for(char &C : Upper)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
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

static bool ExecuteSql(DbHandle &Db, const std::string &Sql) {
	try {
		std::set<std::string> TablesBefore;
		Db.Interp.EnsurePrimaryDatabaseOpened();
		Database *Primary = Db.Interp.PrimaryDatabase();
		if(Primary != nullptr) {
			Primary->WithExclusiveBytecodeLock([&]() {
				for(const auto &Pr : Primary->Tables_)
					TablesBefore.insert(Pr.first);
			});
		}

		AstralDB::SQL::Parser Parser(Sql);
		AstralDB::SQL::Bytecode Code = AstralDB::SQL::BuildBytecode(Db.Logger.get(), AstralDB::SQL::OptimizationLevel::None);
		Db.Interp.Execute(Code);
		Db.LastError.clear();
		return true;
	} catch(const std::exception &Ex) {
		Db.LastError = Ex.what();
		return false;
	}
}

static void CaptureSelectRows(StmtHandle &Stmt, const std::string &Sql) {
	Stmt.Db->Interp.EnsurePrimaryDatabaseOpened();
	Database *Primary = Stmt.Db->Interp.PrimaryDatabase();
	if(Primary == nullptr)
		return;
	if(!ExecuteSql(*Stmt.Db, Sql))
		return;
	const std::string OutTable = ExtractFromTableName(Sql);
	if(OutTable.empty()) {
		Stmt.Db->LastError = "astraldb_prepare could not resolve SELECT source table.";
		return;
	}
	auto Rows = Primary->Select(OutTable, [](const Database::Item &) { return true; }).get();
	Stmt.Rows = std::move(Rows);
	auto Schema = Primary->TableSchemaSnapshot(OutTable);
	if(Schema.has_value()) {
		for(const auto &C : *Schema)
			Stmt.ColumnOrder.push_back(C.Name);
	}
}

} // namespace

struct astraldb_t {
	DbHandle Impl;
	explicit astraldb_t(std::filesystem::path Path) : Impl(std::move(Path)) {}
};

struct astraldb_stmt_t {
	StmtHandle Impl;
};

extern "C" {

int astraldb_open(const char *Path, astraldb_t **OutDb) {
	if(Path == nullptr || OutDb == nullptr)
		return -1;
	try {
		*OutDb = nullptr;
		*OutDb = new astraldb_t(Path);
		return 0;
	} catch(...) {
		*OutDb = nullptr;
		return -1;
	}
}

void astraldb_close(astraldb_t *Db) {
	delete Db;
}

int astraldb_exec(astraldb_t *Db, const char *Sql) {
	if(Db == nullptr || Sql == nullptr)
		return -1;
	return ExecuteSql(Db->Impl, Sql) ? 0 : -1;
}

const char *astraldb_last_error(const astraldb_t *Db) {
	if(Db == nullptr)
		return "astraldb handle is null";
	return Db->Impl.LastError.c_str();
}

int astraldb_prepare(astraldb_t *Db, const char *Sql, astraldb_stmt_t **OutStmt) {
	if(Db == nullptr || Sql == nullptr || OutStmt == nullptr)
		return -1;
	try {
		*OutStmt = nullptr;
		std::string Query(Sql);
		std::size_t Pos = 0;
		while(Pos < Query.size() && std::isspace(static_cast<unsigned char>(Query[Pos])))
			++Pos;
		std::string Head;
		for(; Pos < Query.size() && std::isalpha(static_cast<unsigned char>(Query[Pos])); ++Pos)
			Head.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(Query[Pos]))));
		if(Head != "SELECT") {
			Db->Impl.LastError = "astraldb_prepare currently supports SELECT statements only.";
			return -1;
		}
		auto *Stmt = new astraldb_stmt_t();
		Stmt->Impl.Db = &Db->Impl;
		CaptureSelectRows(Stmt->Impl, Query);
		if(!Db->Impl.LastError.empty()) {
			delete Stmt;
			return -1;
		}
		Db->Impl.LastError.clear();
		*OutStmt = Stmt;
		return 0;
	} catch(const std::exception &Ex) {
		Db->Impl.LastError = Ex.what();
		*OutStmt = nullptr;
		return -1;
	}
}

int astraldb_stmt_step(astraldb_stmt_t *Stmt) {
	if(Stmt == nullptr)
		return -1;
	if(Stmt->Impl.Cursor >= Stmt->Impl.Rows.size())
		return 0;
	++Stmt->Impl.Cursor;
	return 1;
}

int astraldb_stmt_column_count(const astraldb_stmt_t *Stmt) {
	if(Stmt == nullptr)
		return -1;
	return static_cast<int>(Stmt->Impl.ColumnOrder.size());
}

const char *astraldb_stmt_column_name(const astraldb_stmt_t *Stmt, int Col) {
	if(Stmt == nullptr || Col < 0 || static_cast<std::size_t>(Col) >= Stmt->Impl.ColumnOrder.size())
		return nullptr;
	return Stmt->Impl.ColumnOrder[static_cast<std::size_t>(Col)].c_str();
}

const char *astraldb_stmt_column_text(const astraldb_stmt_t *StmtIn, int Col) {
	if(StmtIn == nullptr || Col < 0)
		return nullptr;
	auto *Stmt = const_cast<astraldb_stmt_t *>(StmtIn);
	if(Stmt->Impl.Cursor == 0 || Stmt->Impl.Cursor > Stmt->Impl.Rows.size())
		return nullptr;
	const auto &Row = Stmt->Impl.Rows[Stmt->Impl.Cursor - 1];
	const std::size_t Idx = static_cast<std::size_t>(Col);
	if(Idx >= Stmt->Impl.ColumnOrder.size())
		return nullptr;
	auto It = Row.find(Stmt->Impl.ColumnOrder[Idx]);
	if(It == Row.end())
		return nullptr;
	Stmt->Impl.LastValue = It->second;
	return Stmt->Impl.LastValue.c_str();
}

void astraldb_stmt_finalize(astraldb_stmt_t *Stmt) {
	delete Stmt;
}

} // extern "C"
