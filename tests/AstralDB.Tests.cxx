/* Database before doctest so schema types are parsed before short macro names leak in. */
#include <Database/AdvancedTypes.hxx>
#include <Database/Database.hxx>
#include <Database/ColumnarStorage.hxx>
#include <Database/HybridStorageScheduler.hxx>
#include <Database/HybridTable.hxx>
#include <Database/Superfetch.hxx>
#include <Database/MathSci.hxx>
#include <Database/MathSciAutograd.hxx>
#include <Database/MathSciSignal.hxx>
#include <Database/MathSciSolves.hxx>
#include <Database/MathSciClassify.hxx>
#include <Database/MathSciNlp.hxx>
#include <Database/MathSciEmbeddings.hxx>
#include <Database/MathSciComplex.hxx>
#include <Database/GeoSpatial.hxx>
#include <DS/Geometry2D.hxx>
#include <IO/MemoryGuard.hxx>
#include <Database/TimeSeriesCompression.hxx>
#include <Database/WriteAheadLog.hxx>
#include <DS/ErrorCorrection.hxx>
#include <IO/Logger.hxx>
#include <IO/SIMD.hxx>
#include <SQL/SQL.hxx>
#include <SQL/DialectCompat.hxx>
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include <SQL/BytecodeFormat.hxx>
#include <SQL/BytecodeInspect.hxx>
#include <SQL/BytecodeDebug.hxx>
#include <SQL/BytecodeProcedures.hxx>
#include <SQL/BytecodeTriggers.hxx>
#include <SQL/ProcedureParser.hxx>
#include <astraldb/astraldb.h>
#include "AstralTestHelpers.hxx"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <set>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

fs::path UniqueTempDir(const char *Prefix) {
	const auto Nano =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count();
	std::random_device Rd;
	fs::path Base =
	    fs::temp_directory_path() / (std::string(Prefix) + std::to_string(Nano) + "_" + std::to_string(Rd()));
	fs::create_directories(Base);
	return Base;
}

fs::path RepoRoot() {
	for(fs::path P = fs::current_path(); !P.empty(); P = P.parent_path()) {
		std::error_code Ec;
		if(fs::is_directory(P / "examples", Ec) && fs::is_directory(P / "sources", Ec))
			return fs::weakly_canonical(P);
	}
	fs::path File = fs::path(__FILE__);
	if(!File.is_absolute())
		File = fs::absolute(File);
	return fs::weakly_canonical(File.parent_path().parent_path());
}

std::string ReadExampleFile(const fs::path &Path) {
	std::ifstream In(Path, std::ios::binary);
	REQUIRE(In);
	std::ostringstream O;
	O << In.rdbuf();
	std::string S = std::move(O).str();
	if(S.size() >= 3 && static_cast<unsigned char>(S[0]) == 0xEF && static_cast<unsigned char>(S[1]) == 0xBB &&
	   static_cast<unsigned char>(S[2]) == 0xBF)
		S.erase(0, 3);
	return S;
}

struct ScopedCwd {
	fs::path Previous;
	explicit ScopedCwd(fs::path Dir) {
		std::error_code Ec;
		Previous = fs::current_path(Ec);
		fs::current_path(std::move(Dir), Ec);
	}
	~ScopedCwd() {
		std::error_code Ec;
		fs::current_path(std::move(Previous), Ec);
	}
};

void RemoveEphemeralDb(const fs::path &Dir) {
	std::error_code Ec;
	fs::remove(Dir / "astral.db", Ec);
	fs::remove(Dir / "astral.db.wal", Ec);
}

/** Locked read of all rows (avoids racing the async flush worker on public \c Tables_). */
AstralDB::Database::Table AllRows(const AstralDB::Database *Db, const std::string &Table) {
	REQUIRE(Db != nullptr);
	return Db->Select(Table, [](const AstralDB::Database::Item &) { return true; }).get();
}

bool TableExists(const AstralDB::Database *Db, const std::string &Name) {
	return Db != nullptr && Db->TableSchemaSnapshot(Name).has_value();
}

/** Perf-tier SQL harnesses (see \c ctest -L perf); excluded from the sub-1s contract sweep. */
bool IsPerfHarness(const std::string &FileName) {
	if(FileName.starts_with("benchmark_") || FileName.starts_with("stress_") || FileName == "nuke.sql" ||
	   FileName == "benchmark_torture.sql" || FileName == "torture_unhinged.sql" || FileName == "torture_advanced.sql")
		return true;
	return FileName.starts_with("torture_");
}

bool RunAllExamplesInDefaultSuite() {
	const char *Env = std::getenv("ASTRALDB_RUN_ALL_EXAMPLES");
	if(Env == nullptr)
		return false;
	const std::string V = Env;
	return V == "1" || V == "true" || V == "TRUE" || V == "yes" || V == "YES";
}

bool RunExamplesSweep() {
	const char *Env = std::getenv("ASTRALDB_RUN_EXAMPLES");
	if(Env == nullptr)
		return false;
	const std::string V = Env;
	return V == "1" || V == "true" || V == "TRUE" || V == "yes" || V == "YES";
}

bool IsDefaultSlowExample(const std::string &FileName) {
	return FileName == "sql92_03_coverage.sql" || FileName == "sql_math_sci.sql" ||
	       FileName == "sql_window_frames.sql" || FileName == "sql_dialect_compat.sql";
}

} // namespace

TEST_CASE("WriteAheadLog: embedding catalog survives sync and WAL") {
	fs::path Dir = UniqueTempDir("astral_emb_wal_");
	fs::path DbPath = Dir / "emb.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::Database::Schema Sch;
		AstralDB::Database::Column Tok;
		Tok.Name = "tok";
		Tok.DefaultValue = "TEXT";
		AstralDB::Database::Column Vec;
		Vec.Name = "vec";
		Vec.DefaultValue = "TEXT";
		Sch.push_back(Tok);
		Sch.push_back(Vec);
		Db.CreateTable("vocab", Sch).get();
		Db.Insert("vocab", {{"tok", "a"}, {"vec", "CV[2]:1,0,0,1"}}).get();
		Db.Insert("vocab", {{"tok", "b"}, {"vec", "CV[2]:0,1,1,0"}}).get();
		Db.RegisterEmbedding("emb", "vocab", "tok", "vec");
		REQUIRE(Db.EmbeddingWireCell("emb").has_value());
		Db.SyncToFile();
	}
	REQUIRE(fs::exists(DbPath));
	AstralDB::Database Db2(DbPath, nullptr);
	REQUIRE(Db2.EmbeddingWireCell("emb").has_value());
	REQUIRE(Db2.EmbeddingWireCell("emb")->rfind("EC[2,2]:", 0) == 0);
	const auto Lookup = AstralDB::MathSciEmbeddings::LookupCellFromReal(*Db2.EmbeddingWireCell("emb"), "a", &Db2);
	REQUIRE(Lookup.has_value());
}

TEST_CASE("WriteAheadLog: recover schema and row after reopen") {
	AstralTest::PerfSection Perf("Wal reopen");
	fs::path Dir = UniqueTempDir("astral_wal_");
	fs::path DbPath = Dir / "waltest.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::Database::Schema Sch;
		AstralDB::Database::Column C;
		C.Name = "phrase";
		C.DefaultValue = "TEXT";
		C.IsPrimaryKey = false;
		C.IsUnique = false;
		C.IsNotNull = false;
		Sch.push_back(C);
		Db.CreateTable("hello", Sch).get();
		Db.Insert("hello", {{"phrase", "recovery-check"}}).get();
		REQUIRE(AllRows(&Db, "hello").size() == 1);
	}
	REQUIRE(fs::exists(DbPath.string() + ".wal"));
	AstralDB::Database Db2(DbPath, nullptr);
	REQUIRE(TableExists(&Db2, "hello"));
	REQUIRE(AllRows(&Db2, "hello").size() == 1);
	REQUIRE(AllRows(&Db2, "hello")[0].at("phrase") == "recovery-check");
}

TEST_CASE("WriteAheadLog: encrypted W1 lines decode on replay") {
	fs::path Dir = UniqueTempDir("astral_wal_enc_");
	fs::path DbPath = Dir / "wal_enc.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	{
		AstralDB::WriteAheadLog Wal(DbPath);
		Wal.AppendLine("T|enc_tbl|1|x|INT|0|0|0|-|-");
		Wal.Flush();
		std::ifstream WalIn(DbPath.string() + ".wal");
		std::string WalLine;
		REQUIRE(std::getline(WalIn, WalLine));
		REQUIRE(WalLine.size() >= 3);
		REQUIRE(WalLine.compare(0, 3, "W1|") == 0);
	}
	AstralDB::Database Db(DbPath, nullptr);
	REQUIRE(Db.TableSchemaSnapshot("enc_tbl").has_value());
}

TEST_CASE("DS ErrorCorrection: single-byte flip is recoverable") {
	const std::string Msg = "T|hello|1|phrase|TEXT|0|0|0|-|-|I|hello|1|phrase|wal";
	std::string Prot = AstralDB::DS::ErrorCorrection::Protect(Msg);
	REQUIRE(Prot.size() > 4);
	Prot[9] ^= 0x37;
	const auto Rec = AstralDB::DS::ErrorCorrection::Recover(Prot);
	REQUIRE(Rec.has_value());
	REQUIRE(*Rec == Msg);
}

TEST_CASE("Foreign keys persist across database reopen") {
	fs::path Dir = UniqueTempDir("astral_fk_reopen_");
	fs::path DbPath = Dir / "fkpersist.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::Database::Schema Par;
		AstralDB::Database::Column Pk;
		Pk.Name = "id";
		Pk.DefaultValue = "INT";
		Pk.IsPrimaryKey = true;
		Pk.IsUnique = false;
		Pk.IsNotNull = false;
		Par.push_back(Pk);
		Db.CreateTable("par_fk_persist", Par).get();
		AstralDB::Database::Schema Chi;
		AstralDB::Database::Column Ch;
		Ch.Name = "pid";
		Ch.DefaultValue = "INT";
		Ch.IsPrimaryKey = false;
		Ch.IsUnique = false;
		Ch.IsNotNull = false;
		Chi.push_back(Ch);
		Db.CreateTable("chi_fk_persist", Chi).get();
		AstralDB::ForeignKey Fk;
		Fk.ColumnName = "pid";
		Fk.ReferencedTable = "par_fk_persist";
		Fk.ReferencedColumn = "id";
		Fk.OnDelete = AstralDB::ReferentialAction::Restrict;
		Db.AddForeignKey("chi_fk_persist", Fk).get();
		Db.Insert("par_fk_persist", {{"id", "1"}}).get();
		Db.Insert("chi_fk_persist", {{"pid", "1"}}).get();
		Db.SyncToFile();
	}
	AstralDB::Database Db2(DbPath, nullptr);
	REQUIRE_THROWS_AS(Db2.Insert("chi_fk_persist", {{"pid", "99"}}).get(), std::runtime_error);
}

TEST_CASE("SQL: CREATE TABLE and INSERT implicit columns execute") {
	AstralDB::SQL::SetParserDiagnostics(false);
	AstralTest::PerfSection Perf("CREATE INSERT");
	fs::path Dir = UniqueTempDir("astral_sql_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_t.log").string(), false);
	const char *Query =
	    "CREATE TABLE t (id INT, name TEXT); INSERT INTO t VALUES (1, 'alpha');";
	AstralDB::SQL::Parser Parser(Query);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.Execute(Code);
	const AstralDB::Database *Db = Interp.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(TableExists(Db, "t"));
	REQUIRE(AllRows(Db, "t").size() == 1);
	REQUIRE(AllRows(Db, "t")[0].at("id") == "1");
	REQUIRE(AllRows(Db, "t")[0].at("name") == "alpha");
}

TEST_CASE("SQL: multi-row INSERT emits multiple logical inserts") {
	AstralDB::SQL::SetParserDiagnostics(false);
	AstralTest::PerfSection Perf("multi INSERT");
	fs::path Dir = UniqueTempDir("astral_sql_multi_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_t.log").string(), false);
	const char *Query = "CREATE TABLE u (k INT, v TEXT); INSERT INTO u VALUES (1, 'a'), (2, 'b');";
	AstralDB::SQL::Parser P(Query);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.Execute(Code);
	const AstralDB::Database *Db = Interp.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "u").size() == 2);
}

TEST_CASE("SQL: INSERT BULK uses INSERT_BULK opcode and fills rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_bulk_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "bulk.log").string(), false);
	const char *Query =
	    "CREATE TABLE b (id INT, a INT, b TEXT, c TEXT, d TEXT); INSERT INTO b BULK 2500 START 1 STEP 1;";
	AstralDB::SQL::Parser P(Query);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE(Code.size() >= 2);
	bool SawBulk = false;
	for(const auto &Inst : Code)
		if(Inst.Opcode_ == AstralDB::SQL::Opcode::INSERT_BULK)
			SawBulk = true;
	REQUIRE(SawBulk);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.Execute(Code);
	const AstralDB::Database *Db = Interp.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "b").size() == 2500);
}

TEST_CASE("SQL: UPDATE and DELETE parse and run") {
	AstralDB::SQL::SetParserDiagnostics(false);
	AstralTest::PerfSection Perf("UPDATE DELETE");
	fs::path Dir = UniqueTempDir("astral_ud_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "l.log").string(), false);
	const char *QUp = "CREATE TABLE m (id INT, v TEXT); INSERT INTO m VALUES (1, 'old'); "
	                  "UPDATE m SET v = 'new' WHERE id = 1;";
	AstralDB::SQL::Parser P1(QUp);
	AstralDB::SQL::Bytecode Code1 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code1);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "m").size() == 1);
	REQUIRE(AllRows(Db, "m")[0].at("v") == "new");

	I.Reset();
	const char *QDel = "DELETE FROM m WHERE id = 1;";
	AstralDB::SQL::Parser P2(QDel);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	I.Execute(Code2);
	REQUIRE(AllRows(Db, "m").empty());
}

TEST_CASE("SQL: multi-column SELECT finalizes") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_selmc_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "l.log").string(), false);
	const char *Q =
	    "CREATE TABLE s (id INT, k INT); INSERT INTO s VALUES (1, 10); INSERT INTO s VALUES (2, 20); "
	    "SELECT id, k FROM s;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE(!Code.empty());
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
}

TEST_CASE("SQL: SHOW TABLES, DESCRIBE, and INFORMATION_SCHEMA execute") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_meta_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "meta.log").string(), false);
	const char *Q = "CREATE TABLE tmeta (id INT, name TEXT); "
	                "SHOW TABLES; "
	                "DESCRIBE tmeta; "
	                "SELECT table_name FROM information_schema.tables;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(TableExists(Db, "__astral_show_tables"));
	REQUIRE(TableExists(Db, "__astral_describe"));
	const auto ShowRows = AllRows(Db, "__astral_show_tables");
	bool FoundMeta = false;
	for(const auto &R : ShowRows) {
		auto It = R.find("table_name");
		if(It != R.end() && It->second == "tmeta") {
			FoundMeta = true;
			break;
		}
	}
	REQUIRE(FoundMeta);
}

TEST_CASE("SQL: NOW and CURRENT date/time parse to scalar bytecode") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_timefn_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "timefn.log").string(), false);
	const char *Q = "CREATE TABLE tt (id INT); INSERT INTO tt VALUES (1); "
	                "SELECT NOW() AS n, CURRENT_DATE AS d, CURRENT_TIME AS t FROM tt;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(TableExists(Db, "tt"));
}

TEST_CASE("SQL: SET TRANSACTION ISOLATION LEVEL SERIALIZABLE parses") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_txiso_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "txiso.log").string(), false);
	const char *Q = "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE; BEGIN; COMMIT;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	bool SawSetIso = false;
	for(const auto &Inst : Code)
		if(Inst.Opcode_ == AstralDB::SQL::Opcode::SET_TRANSACTION_ISOLATION)
			SawSetIso = true;
	REQUIRE(SawSetIso);
}

TEST_CASE("SQL: DROP TABLE, INSERT column list, AND WHERE, DISTINCT, LIMIT syntax") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ext_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ex.log").string(), false);
	const char *Q =
	    "CREATE TABLE mx (id INT, x INT); "
	    "INSERT INTO mx (x, id) VALUES (10, 1), (11, 2), (12, 2); "
	    "DELETE FROM mx WHERE id = 2 AND x = 11; "
	    "CREATE TABLE lim2 (seq INT); "
	    "INSERT INTO lim2 VALUES (100), (200), (300), (400); "
	    "DROP TABLE mx; "
	    "SELECT DISTINCT seq FROM lim2 WHERE seq >= 200 ORDER BY seq ASC LIMIT 0, 2;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE_FALSE(TableExists(Db, "mx"));
	REQUIRE(TableExists(Db, "lim2"));
	const auto Lim = AllRows(Db, "lim2");
	REQUIRE(Lim.size() == size_t(2));
	REQUIRE(Lim.at(0).at("seq") == "200");
	REQUIRE(Lim.at(1).at("seq") == "300");
}

TEST_CASE("Database: bundle JSON export and import roundtrip") {
	fs::path Dir = UniqueTempDir("astral_bundle_");
	fs::path Db1 = Dir / "one.db";
	fs::path Db2 = Dir / "two.db";
	fs::path Bundle = Dir / "bundle.json";
	std::error_code Ec;
	fs::remove(Db1, Ec);
	fs::remove(std::string(Db1.string()) + ".wal", Ec);
	fs::remove(Db2, Ec);
	fs::remove(std::string(Db2.string()) + ".wal", Ec);
	{
		AstralDB::Database Db(Db1, nullptr);
		AstralDB::Database::Schema Sch;
		AstralDB::Database::Column C;
		C.Name = "n";
		C.DefaultValue = "INT";
		Sch.push_back(C);
		Db.CreateTable("t", Sch).get();
		Db.Insert("t", {{"n", "42"}}).get();
		REQUIRE(Db.ExportBundle(Bundle, "json"));
	}
	{
		AstralDB::Database Db2o(Db2, nullptr);
		REQUIRE(Db2o.ImportBundle(Bundle, "json"));
		REQUIRE(TableExists(&Db2o, "t"));
		REQUIRE(AllRows(&Db2o, "t").size() == 1);
		REQUIRE(AllRows(&Db2o, "t")[0].at("n") == "42");
	}
}

TEST_CASE("Database: ConvertTabularFiles csv to tsv") {
	fs::path Dir = UniqueTempDir("astral_conv_");
	fs::path Csv = Dir / "a.csv";
	fs::path Tsv = Dir / "a.tsv";
	{
		std::ofstream O(Csv);
		REQUIRE(O);
		O << "c1,c2\n1,two\n";
	}
	REQUIRE(AstralDB::Database::ConvertTabularFiles(Csv, Tsv, "csv", "tsv"));
	std::ifstream In(Tsv);
	REQUIRE(In);
	std::string Line;
	REQUIRE(std::getline(In, Line));
	REQUIRE(Line.find('\t') != std::string::npos);
}

TEST_CASE("SQL: SAVEPOINT and ROLLBACK TO restore snapshot") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_sp_");
	ScopedCwd Cwd(Dir);
	fs::path DbPath = Dir / "savepoint.db";
	RemoveEphemeralDb(Dir);
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	AstralDB::Logger Log((Dir / "sp.log").string(), false);
	const char *Q = "CREATE TABLE sp_t (id INT); INSERT INTO sp_t VALUES (99); SAVEPOINT a; INSERT INTO sp_t "
	                "VALUES (100); ROLLBACK TO SAVEPOINT a;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.DatabasePath(DbPath);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "sp_t").size() == 1);
	REQUIRE(AllRows(Db, "sp_t")[0].at("id") == "99");
}

TEST_CASE("SQL: EXPORT and IMPORT DATABASE via JSON bundle") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_sqlex_");
	ScopedCwd Cwd(Dir);
	fs::path DbPath = Dir / "edb.db";
	RemoveEphemeralDb(Dir);
	{
		std::error_code Ec;
		fs::remove(DbPath, Ec);
		fs::remove(DbPath.string() + ".wal", Ec);
	}
	AstralDB::Logger Log((Dir / "edx.log").string(), false);
	const char *Q = "CREATE TABLE z (id INT); INSERT INTO z VALUES (7); EXPORT DATABASE TO 'round.json' "
	                "FORMAT JSON; DROP TABLE z; IMPORT DATABASE FROM 'round.json' FORMAT JSON;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.DatabasePath(DbPath);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(TableExists(Db, "z"));
	REQUIRE(AllRows(Db, "z").size() == 1);
	REQUIRE(AllRows(Db, "z")[0].at("id") == "7");
}

TEST_CASE("SQL: GROUP BY COUNT(*) aggregates per key") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_gb_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_g.log").string(), false);
	const char *Q = "CREATE TABLE regions (region TEXT); "
	                "INSERT INTO regions VALUES ('east'), ('east'), ('west'); "
	                "SELECT region, cnt FROM regions GROUP BY region;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(TableExists(Db, "regions"));
	const auto Rows = AllRows(Db, "regions");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("region") == "east");
	REQUIRE(Rows[0].at("cnt") == "2");
	REQUIRE(Rows[1].at("region") == "west");
	REQUIRE(Rows[1].at("cnt") == "1");
}

TEST_CASE("SQL: NOT EXISTS (non-correlated) keeps rows when inner is empty") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ex_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_ex.log").string(), false);
	const char *Q = "CREATE TABLE pr (pk INT); CREATE TABLE cr (fk INT); "
	                "INSERT INTO pr VALUES (1); INSERT INTO pr VALUES (2); "
	                "INSERT INTO cr VALUES (1); "
	                /* Inner WHERE is never satisfied; NOT EXISTS stays true on every outer row. */
	                "SELECT pk FROM pr WHERE NOT EXISTS (SELECT * FROM cr WHERE fk = 999);";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "pr");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("pk") == "1");
	REQUIRE(Rows[1].at("pk") == "2");
}

TEST_CASE("SQL: IN and NOT IN subqueries") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_in_sub_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "insub.log").string(), false);
	const char *Q =
	    "CREATE TABLE pr (pk INT); CREATE TABLE cr (fk INT); "
	    "INSERT INTO pr VALUES (1),(2),(3); INSERT INTO cr VALUES (1),(3); "
	    "SELECT pk FROM pr WHERE pk IN (SELECT fk FROM cr) ORDER BY pk;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "pr");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("pk") == "1");
	REQUIRE(Rows[1].at("pk") == "3");
}

TEST_CASE("SQL: NOT IN literal list") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_notin_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "notin.log").string(), false);
	const char *Q =
	    "CREATE TABLE t (n INT); INSERT INTO t VALUES (1),(2),(3); "
	    "SELECT n FROM t WHERE n NOT IN (2, 99) ORDER BY n;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "t");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("n") == "1");
	REQUIRE(Rows[1].at("n") == "3");
}

TEST_CASE("SQL: SUM OVER RANGE BETWEEN frame") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_wrange_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "wrange.log").string(), false);
	const char *Q =
	    "CREATE TABLE vals (x INT); INSERT INTO vals VALUES (1),(3),(5),(7),(9); "
	    "SELECT x, SUM(x) OVER (ORDER BY x ASC RANGE BETWEEN 2 PRECEDING AND CURRENT ROW) AS rsum "
	    "FROM vals ORDER BY x ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "vals");
	REQUIRE(Rows.size() == 5);
	REQUIRE(Rows[0].at("rsum") == "1");
	REQUIRE(Rows[1].at("rsum") == "4");
	REQUIRE(Rows[2].at("rsum") == "8");
	REQUIRE(Rows[3].at("rsum") == "12");
	REQUIRE(Rows[4].at("rsum") == "16");
}

TEST_CASE("SQL: correlated EXISTS / NOT EXISTS (column = column in inner WHERE)") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ex_corr_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_exc.log").string(), false);
	{
		const char *Q = "CREATE TABLE pr (pk INT); CREATE TABLE cr (fk INT); "
		               "INSERT INTO pr VALUES (1); INSERT INTO pr VALUES (2); INSERT INTO cr VALUES (1); "
		               "DELETE FROM pr WHERE NOT EXISTS (SELECT * FROM cr WHERE fk = pk);";
		AstralDB::SQL::Parser P(Q);
		AstralDB::SQL::Bytecode Code =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		AstralDB::SQL::BytecodeInterpreter I(&Log);
		I.Execute(Code);
		const AstralDB::Database *Db = I.PrimaryDatabase();
		REQUIRE(Db != nullptr);
		const auto Rows = AllRows(Db, "pr");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("pk") == "1");
	}
	RemoveEphemeralDb(Dir);
	{
		const char *Q2 = "CREATE TABLE pr (pk INT); CREATE TABLE cr (fk INT); "
		                "INSERT INTO pr VALUES (1); INSERT INTO pr VALUES (2); INSERT INTO cr VALUES (1); "
		                "DELETE FROM pr WHERE EXISTS (SELECT * FROM cr WHERE fk = pk);";
		AstralDB::SQL::Parser P2(Q2);
		AstralDB::SQL::Bytecode Code2 =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		AstralDB::SQL::BytecodeInterpreter I2(&Log);
		I2.Execute(Code2);
		const AstralDB::Database *Db2 = I2.PrimaryDatabase();
		REQUIRE(Db2 != nullptr);
		const auto Rows2 = AllRows(Db2, "pr");
		REQUIRE(Rows2.size() == 1);
		REQUIRE(Rows2[0].at("pk") == "2");
	}
}

TEST_CASE("SQL: ANY SOME ALL quantified scalar subqueries") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_qsub_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_qsub.log").string(), false);
	const char *Q =
	    "CREATE TABLE lhs (x INT); INSERT INTO lhs VALUES (1),(5),(9); "
	    "CREATE TABLE rhs (y INT); INSERT INTO rhs VALUES (3),(7); "
	    "SELECT x FROM lhs WHERE x > ANY (SELECT y FROM rhs) ORDER BY x ASC;";
	AstralDB::SQL::Parser P(Q);
	auto Code = AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const auto *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	auto Rows = AllRows(Db, "lhs");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("x") == "5");
	REQUIRE(Rows[1].at("x") == "9");

	const char *Q2 =
	    "CREATE TABLE lhs2 (x INT); INSERT INTO lhs2 VALUES (1),(5),(9); "
	    "CREATE TABLE rhs2 (y INT); INSERT INTO rhs2 VALUES (3),(7); "
	    "SELECT x FROM lhs2 WHERE x > SOME (SELECT y FROM rhs2) ORDER BY x ASC;";
	AstralDB::SQL::Parser P2(Q2);
	REQUIRE_NOTHROW(I.Execute(AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None)));
	Rows = AllRows(Db, "lhs2");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("x") == "5");
	REQUIRE(Rows[1].at("x") == "9");

	const char *Q3 =
	    "CREATE TABLE lhs3 (x INT); INSERT INTO lhs3 VALUES (1),(5),(9); "
	    "CREATE TABLE rhs3 (y INT); INSERT INTO rhs3 VALUES (3),(7); "
	    "SELECT x FROM lhs3 WHERE x > ALL (SELECT y FROM rhs3) ORDER BY x ASC;";
	AstralDB::SQL::Parser P3(Q3);
	REQUIRE_NOTHROW(I.Execute(AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None)));
	Rows = AllRows(Db, "lhs3");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("x") == "9");
}

TEST_CASE("SQL: row constructor comparisons") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_rowcmp_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_rowcmp.log").string(), false);
	const char *Q =
	    "CREATE TABLE rc (a INT, b INT); INSERT INTO rc VALUES (1,2),(1,3),(2,1); "
	    "SELECT a, b FROM rc WHERE (a, b) >= (1, 3) ORDER BY a ASC, b ASC;";
	AstralDB::SQL::Parser P(Q);
	auto Code = AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const auto *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "rc");
	REQUIRE(Rows.size() == 2);
	std::vector<std::pair<int, int>> Got;
	for(const auto &R : Rows)
		Got.emplace_back(std::stoi(R.at("a")), std::stoi(R.at("b")));
	std::sort(Got.begin(), Got.end());
	REQUIRE(Got[0] == std::make_pair(1, 3));
	REQUIRE(Got[1] == std::make_pair(2, 1));
}

TEST_CASE("SQL: CURRENT_TIMESTAMP and timezone conversion functions") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_tz_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_tz.log").string(), false);
	const char *Q =
	    "CREATE TABLE tz (base TEXT); INSERT INTO tz VALUES ('2024-01-01T00:00:00Z'); "
	    "SELECT CURRENT_TIMESTAMP() AS ct, AT_TIME_ZONE(base, '+02:00') AS t1, "
	    "CONVERT_TZ(base, '+00:00', '+03:00') AS t2 FROM tz;";
	AstralDB::SQL::Parser P(Q);
	auto Code = AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const auto *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "tz");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].find("ct") != Rows[0].end());
	REQUIRE(Rows[0].at("ct").find('T') != std::string::npos);
	REQUIRE(Rows[0].at("t1") == "2024-01-01T02:00:00Z");
	REQUIRE(Rows[0].at("t2") == "2024-01-01T03:00:00Z");
}

TEST_CASE("SQL: WITH clones source and evaluates main SELECT on CTE") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_w_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_w.log").string(), false);
	const char *Q = "CREATE TABLE numsrc (id INT); INSERT INTO numsrc VALUES (5), (15), (25); "
	                "WITH pick AS (SELECT id FROM numsrc WHERE id > 10) SELECT id FROM pick ORDER BY "
	                "id ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(TableExists(Db, "numsrc"));
	REQUIRE(AllRows(Db, "numsrc").size() == size_t(3));
	REQUIRE(TableExists(Db, "__astral_cte_0_pick"));
	const auto Cte = AllRows(Db, "__astral_cte_0_pick");
	REQUIRE(Cte.size() == 2);
	REQUIRE(Cte.at(0).at("id") == "15");
	REQUIRE(Cte.at(1).at("id") == "25");
}

TEST_CASE("SQL: ROW_NUMBER OVER (ORDER BY) assigns deterministic ranks") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_rn_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_rn.log").string(), false);
	const char *Q = "CREATE TABLE ranked (seq INT); INSERT INTO ranked VALUES (30), (10), (20); "
	                "SELECT ROW_NUMBER() OVER (ORDER BY seq DESC) AS rnk FROM ranked;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "ranked");
	REQUIRE(Rows.size() == 3);
	REQUIRE(Rows[0].at("seq") == "30");
	REQUIRE(Rows[0].at("rnk") == "1");
	REQUIRE(Rows[1].at("seq") == "20");
	REQUIRE(Rows[1].at("rnk") == "2");
	REQUIRE(Rows[2].at("seq") == "10");
	REQUIRE(Rows[2].at("rnk") == "3");
}

TEST_CASE("SQL: ROW_NUMBER OVER (PARTITION BY ... ORDER BY) resets rank per partition") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_rn_part_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_rnp.log").string(), false);
	const char *Q =
	    "CREATE TABLE parts (dept TEXT, seq INT); "
	    /* Values chosen so lexical string order matches numeric order (< 10 avoids "10" vs "5"). */
	    "INSERT INTO parts VALUES ('A', 8), ('A', 2), ('B', 4), ('B', 1); "
	    "SELECT ROW_NUMBER() OVER (PARTITION BY dept ORDER BY seq ASC) AS rnk FROM parts;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "parts");
	REQUIRE(Rows.size() == 4);
	REQUIRE(Rows[0].at("dept") == "A");
	REQUIRE(Rows[0].at("seq") == "2");
	REQUIRE(Rows[0].at("rnk") == "1");
	REQUIRE(Rows[1].at("dept") == "A");
	REQUIRE(Rows[1].at("seq") == "8");
	REQUIRE(Rows[1].at("rnk") == "2");
	REQUIRE(Rows[2].at("dept") == "B");
	REQUIRE(Rows[2].at("seq") == "1");
	REQUIRE(Rows[2].at("rnk") == "1");
	REQUIRE(Rows[3].at("dept") == "B");
	REQUIRE(Rows[3].at("seq") == "4");
	REQUIRE(Rows[3].at("rnk") == "2");
}

TEST_CASE("SQL: COUNT(DISTINCT column) within GROUP BY") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_cntd_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_cntd.log").string(), false);
	const char *Q =
	    "CREATE TABLE visits (user_id INT, day_id INT); "
	    "INSERT INTO visits VALUES (1,1),(1,2),(1,2),(2,1); "
	    "SELECT user_id, COUNT(DISTINCT day_id) FROM visits GROUP BY user_id ORDER BY user_id ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "visits");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("user_id") == "1");
	REQUIRE(Rows[0].at("cnt") == "2");
	REQUIRE(Rows[1].at("user_id") == "2");
	REQUIRE(Rows[1].at("cnt") == "1");
}

TEST_CASE("SQL: COALESCE uses first non-null scalar") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_coal_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_coal.log").string(), false);
	const char *Q =
	    "CREATE TABLE c (x INT); INSERT INTO c VALUES (7); "
	    "SELECT COALESCE(NULL, x, 0) AS v FROM c;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "c");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("v") == "7");
}

TEST_CASE("SQL: :: cast NVL2 nested ROWNUM CONNECT BY") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_dialect2_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "dialect2.log").string(), false);
	const char *Setup =
	    "CREATE TABLE t (id INT PRIMARY KEY, name TEXT, nick TEXT); "
	    "INSERT INTO t VALUES (1,'Ada','ada'),(2,'Grace',''); "
	    "SELECT name::TEXT AS nt, NVL2(nick, UPPER(nick), 'x') AS nu FROM t ORDER BY id;";
	AstralDB::SQL::Parser P0(Setup);
	AstralDB::SQL::Bytecode Code0 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code0));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	{
		const auto Rows = AllRows(Db, "t");
		REQUIRE(Rows.size() == 2);
		REQUIRE(Rows[0].at("nt") == "Ada");
		REQUIRE(Rows[0].at("nu") == "ADA");
		REQUIRE(Rows[1].at("nu") == "x");
	}
	const char *Rownum =
	    "SELECT name FROM t WHERE ROWNUM <= 1;";
	AstralDB::SQL::Parser P1(Rownum);
	AstralDB::SQL::Bytecode Code1 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code1));
	{
		const auto Rows = AllRows(Db, "t");
		REQUIRE(Rows.size() == 1);
	}
	const char *Hierarchy =
	    "CREATE TABLE org (id INT, name TEXT, mgr INT); "
	    "INSERT INTO org VALUES (1,'ceo',NULL),(2,'eng',1),(3,'sales',1); "
	    "SELECT name FROM org START WITH mgr IS NULL CONNECT BY PRIOR id = mgr ORDER BY name;";
	AstralDB::SQL::Parser P2(Hierarchy);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code2));
	{
		const auto Rows = AllRows(Db, "org");
		REQUIRE(Rows.size() == 3);
		REQUIRE(Rows[0].at("name") == "ceo");
	}
}

TEST_CASE("SQL: DuckDB lambda COLUMNS intdiv REPLACE") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_duck_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "duck.log").string(), false);
	const char *Setup =
	    "CREATE TABLE n (id INT PRIMARY KEY, a INT, b INT); "
	    "INSERT INTO n VALUES (1,10,3),(2,7,2); "
	    "SELECT id, a // b AS q FROM n ORDER BY id;";
	AstralDB::SQL::Parser P0(Setup);
	AstralDB::SQL::Bytecode Code0 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code0));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	{
		const auto Rows = AllRows(Db, "n");
		REQUIRE(Rows.size() == 2);
		REQUIRE(Rows[0].at("q") == "3");
		REQUIRE(Rows[1].at("q") == "3");
	}
	const char *Cols =
	    "CREATE TABLE w (id INT, x INT, y INT); INSERT INTO w VALUES (1,1,2); SELECT COLUMNS(*) FROM w;";
	AstralDB::SQL::Parser P1(Cols);
	AstralDB::SQL::Bytecode Code1 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code1));
	{
		const auto Rows = AllRows(Db, "w");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].find("id") != Rows[0].end());
		REQUIRE(Rows[0].find("x") != Rows[0].end());
		REQUIRE(Rows[0].find("y") != Rows[0].end());
	}
	const char *Replace =
	    "CREATE TABLE u (id INT PRIMARY KEY, name TEXT); "
	    "INSERT INTO u VALUES (1,'Ada'); "
	    "REPLACE INTO u VALUES (1,'Grace'); "
	    "SELECT name FROM u;";
	AstralDB::SQL::Parser P2(Replace);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code2));
	{
		const auto Rows = AllRows(Db, "u");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("name") == "Grace");
	}
}

TEST_CASE("SQL: Postgres RETURNING + SQLite INTEGER PRIMARY KEY") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ret_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ret.log").string(), false);

	AstralDB::SQL::BytecodeInterpreter I(&Log);

	const char *Create =
	    "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);";
	AstralDB::SQL::Parser P0(Create);
	AstralDB::SQL::Bytecode C0 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(C0));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);

	const char *Insert =
	    "INSERT INTO t(name) VALUES ('Ada') RETURNING id, name;";
	AstralDB::SQL::Parser P1(Insert);
	AstralDB::SQL::Bytecode Code1 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code1));
	{
		const auto Rows = AllRows(Db, "__astral_returning");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("id") == "1");
		REQUIRE(Rows[0].at("name") == "Ada");
	}

	const char *Update =
	    "UPDATE t SET name = 'Grace' WHERE id = 1 RETURNING id, name;";
	AstralDB::SQL::Parser P2(Update);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code2));
	{
		const auto Rows = AllRows(Db, "__astral_returning");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("id") == "1");
		REQUIRE(Rows[0].at("name") == "Grace");
	}

	const char *Delete =
	    "DELETE FROM t WHERE id = 1 RETURNING *;";
	AstralDB::SQL::Parser P3(Delete);
	AstralDB::SQL::Bytecode Code3 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code3));
	{
		const auto Rows = AllRows(Db, "__astral_returning");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("id") == "1");
		REQUIRE(Rows[0].at("name") == "Grace");
	}
}

TEST_CASE("SQL: SQLite PostgreSQL Oracle dialect compat") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_dialect_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "dialect.log").string(), false);
	const char *Setup =
	    "CREATE TABLE t (id SERIAL PRIMARY KEY, name TEXT, nick TEXT); "
	    "INSERT INTO t (name, nick) VALUES ('Ada','ada'),('Grace',''); "
	    "SELECT IFNULL(nick, name) AS disp FROM t ORDER BY id;";
	AstralDB::SQL::Parser P0(Setup);
	AstralDB::SQL::Bytecode Code0 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code0));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	{
		const auto Rows = AllRows(Db, "t");
		REQUIRE(Rows.size() == 2);
		REQUIRE(Rows[0].at("disp") == "ada");
		REQUIRE(Rows[1].at("disp") == "Grace");
	}
	const char *Pred =
	    "SELECT name FROM t WHERE name ILIKE 'ad%' ORDER BY name;";
	AstralDB::SQL::Parser P1(Pred);
	AstralDB::SQL::Bytecode Code1 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code1));
	{
		const auto Rows = AllRows(Db, "t");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("name") == "Ada");
	}
	const char *Dual = "SELECT 'ok' AS flag FROM DUAL;";
	AstralDB::SQL::Parser P2(Dual);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code2));
	{
		const auto Rows = AllRows(Db, "DUAL");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("flag") == "ok");
	}
	const char *ResetT =
	    "DELETE FROM t; INSERT INTO t (name, nick) VALUES ('Ada','ada'),('Grace','');";
	AstralDB::SQL::Parser Pr(ResetT);
	AstralDB::SQL::Bytecode CodeR =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(CodeR));
	auto RunNamePred = [&](const char *Sql, const std::string &Expected) {
		AstralDB::SQL::Parser Px(Sql);
		AstralDB::SQL::Bytecode Cx =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		REQUIRE_NOTHROW(I.Execute(Cx));
		const auto Rows = AllRows(Db, "t");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("name") == Expected);
	};
	RunNamePred("SELECT name FROM t WHERE name REGEXP '^A' ORDER BY name;", "Ada");
	AstralDB::SQL::Parser Pr2(ResetT);
	REQUIRE_NOTHROW(I.Execute(AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None)));
	RunNamePred("SELECT name FROM t WHERE name ~ 'race' ORDER BY name;", "Grace");
	AstralDB::SQL::Parser Pr3(ResetT);
	REQUIRE_NOTHROW(I.Execute(AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None)));
	RunNamePred("SELECT name FROM t WHERE REGEXP_MATCH(name, '^G') ORDER BY name;", "Grace");
}

TEST_CASE("SQL: SQL-92 through SQL:2003 standard edge cases") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_sqlstd_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "sqlstd.log").string(), false);
	const char *Scalars =
	    "CREATE TABLE n (id INT, tag TEXT, nval INT); "
	    "INSERT INTO n VALUES (1,'',10),(2,'x',20),(3,'',30); "
	    "SELECT id, NULLIF(nval, 20) AS nz, GREATEST(nval, 15) AS hi, LEAST(nval, 25) AS lo FROM n ORDER BY id;";
	AstralDB::SQL::Parser P1(Scalars);
	AstralDB::SQL::Bytecode Code1 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code1));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	{
		const auto Rows = AllRows(Db, "n");
		REQUIRE(Rows.size() == 3);
		REQUIRE(Rows[0].at("nz") == "10");
		REQUIRE(Rows[0].at("hi") == "15");
		REQUIRE(Rows[0].at("lo") == "10");
		REQUIRE(Rows[1].find("nz") == Rows[1].end());
		REQUIRE(Rows[1].at("hi") == "20");
		REQUIRE(Rows[1].at("lo") == "20");
		REQUIRE(Rows[2].at("nz") == "30");
	}
	const char *BetweenNull =
	    "CREATE TABLE b (id INT, nval INT); INSERT INTO b VALUES (1,10),(2,20); "
	    "SELECT id FROM b WHERE nval BETWEEN NULL AND 100;";
	AstralDB::SQL::Parser P2(BetweenNull);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code2));
	REQUIRE(AllRows(Db, "b").empty());
	const char *Predicates =
	    "CREATE TABLE p (id INT, tag TEXT); INSERT INTO p VALUES (1,''),(2,'x'); "
	    "SELECT id FROM p WHERE tag IS NOT NULL ORDER BY id;";
	AstralDB::SQL::Parser P3(Predicates);
	AstralDB::SQL::Bytecode Code3 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code3));
	{
		const auto Rows = AllRows(Db, "p");
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("id") == "2");
	}
}

TEST_CASE("SQL: RANK with ties skips after duplicate order values") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_rank_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_rank.log").string(), false);
	const char *Q =
	    "CREATE TABLE t (x INT); INSERT INTO t VALUES (5),(3),(3),(1); "
	    "SELECT x, RANK() OVER (ORDER BY x ASC) AS r FROM t ORDER BY x ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "t");
	REQUIRE(Rows.size() == 4);
	std::vector<size_t> Order(Rows.size());
	std::iota(Order.begin(), Order.end(), size_t{0});
	std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
		const std::string &Xa = Rows[A].at("x");
		const std::string &Xb = Rows[B].at("x");
		if(Xa != Xb)
			return Xa < Xb;
		return A < B;
	});
	REQUIRE(Rows[Order[0]].at("r") == "1");
	REQUIRE(Rows[Order[1]].at("r") == "2");
	REQUIRE(Rows[Order[2]].at("r") == "2");
	REQUIRE(Rows[Order[3]].at("r") == "4");
}

TEST_CASE("SQL: SUM OVER default running frame") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_wsum_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "wsum.log").string(), false);
	const char *Q =
	    "CREATE TABLE wf (k INT, v INT); "
	    "INSERT INTO wf VALUES (1, 10), (1, 20), (1, 30), (2, 100); "
	    "SELECT k, v, SUM(v) OVER (PARTITION BY k ORDER BY v ASC) AS rs FROM wf;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "wf");
	REQUIRE(Rows.size() == 4);
	REQUIRE(Rows[0].at("k") == "1");
	REQUIRE(Rows[0].at("v") == "10");
	REQUIRE(Rows[0].at("rs") == "10");
	REQUIRE(Rows[1].at("v") == "20");
	REQUIRE(Rows[1].at("rs") == "30");
	REQUIRE(Rows[2].at("v") == "30");
	REQUIRE(Rows[2].at("rs") == "60");
	REQUIRE(Rows[3].at("k") == "2");
	REQUIRE(Rows[3].at("rs") == "100");
}

TEST_CASE("SQL: parse SUM OVER ROWS BETWEEN") {
	AstralDB::SQL::SetParserDiagnostics(false);
	REQUIRE_NOTHROW(AstralDB::SQL::Parser(
	    "SELECT SUM(v) OVER (ORDER BY v ASC ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS s FROM t;"));
	const char *Full =
	    "CREATE TABLE wf2 (v INT); INSERT INTO wf2 VALUES (10), (20), (30), (40); "
	    "SELECT v, SUM(v) OVER (ORDER BY v ASC ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS s FROM wf2;";
	REQUIRE_NOTHROW(AstralDB::SQL::Parser{Full});
}

TEST_CASE("SQL: SUM OVER explicit ROWS BETWEEN frame") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_wrows_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "wrows.log").string(), false);
	const char *Q =
	    "CREATE TABLE wf2 (v INT); "
	    "INSERT INTO wf2 VALUES (10), (20), (30), (40); "
	    "SELECT v, SUM(v) OVER (ORDER BY v ASC ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS s "
	    "FROM wf2;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	int WinOps = 0;
	for(const AstralDB::SQL::Instruction &In : Code) {
		if(In.Opcode_ == AstralDB::SQL::Opcode::WINDOW_ROW_NUMBER)
			++WinOps;
	}
	REQUIRE(WinOps == 1);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "wf2");
	REQUIRE(Rows.size() == 4);
	for(const auto &Kv : Rows[0])
		INFO("col ", Kv.first, "=", Kv.second);
	REQUIRE(Rows[0].at("v") == "10");
	const std::string &Col =
	    Rows[0].count("s") ? "s" : (Rows[0].count("sum_v") ? "sum_v" : std::string());
	REQUIRE_MESSAGE(!Col.empty(), "window output column missing on row");
	REQUIRE(Rows[0].at(Col) == "10");
	REQUIRE(Rows[1].at("v") == "20");
	REQUIRE(Rows[1].at(Col) == "30");
	REQUIRE(Rows[2].at("v") == "30");
	REQUIRE(Rows[2].at(Col) == "50");
	REQUIRE(Rows[3].at("v") == "40");
	REQUIRE(Rows[3].at(Col) == "70");
}

TEST_CASE("SQL: DENSE_RANK has no gaps after ties") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_drank_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_drank.log").string(), false);
	const char *Q =
	    "CREATE TABLE t (x INT); INSERT INTO t VALUES (5),(3),(3),(1); "
	    "SELECT x, DENSE_RANK() OVER (ORDER BY x ASC) AS d FROM t ORDER BY x ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "t");
	REQUIRE(Rows.size() == 4);
	std::vector<size_t> Order(Rows.size());
	std::iota(Order.begin(), Order.end(), size_t{0});
	std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
		const std::string &Xa = Rows[A].at("x");
		const std::string &Xb = Rows[B].at("x");
		if(Xa != Xb)
			return Xa < Xb;
		return A < B;
	});
	REQUIRE(Rows[Order[0]].at("d") == "1");
	REQUIRE(Rows[Order[1]].at("d") == "2");
	REQUIRE(Rows[Order[2]].at("d") == "2");
	REQUIRE(Rows[Order[3]].at("d") == "3");
}

TEST_CASE("SQL: FIRST_VALUE LAST_VALUE NTH_VALUE window functions") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_wvals_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_wvals.log").string(), false);
	const char *Q =
	    "CREATE TABLE wfvals (k INT, v INT); "
	    "INSERT INTO wfvals VALUES (1, 10), (1, 20), (1, 30), (2, 5), (2, 15); "
	    "SELECT k, v, FIRST_VALUE(v) OVER (PARTITION BY k ORDER BY v ASC) AS fv, "
	    "LAST_VALUE(v) OVER (PARTITION BY k ORDER BY v ASC "
	    "ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS lv, "
	    "NTH_VALUE(v, 2) OVER (PARTITION BY k ORDER BY v ASC "
	    "ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS nv "
	    "FROM wfvals;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "wfvals");
	REQUIRE(Rows.size() == 5);
	std::vector<size_t> Order(Rows.size());
	std::iota(Order.begin(), Order.end(), size_t{0});
	std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
		const int Ka = std::stoi(Rows[A].at("k"));
		const int Kb = std::stoi(Rows[B].at("k"));
		if(Ka != Kb)
			return Ka < Kb;
		return std::stoi(Rows[A].at("v")) < std::stoi(Rows[B].at("v"));
	});
	REQUIRE(Rows[Order[0]].at("fv") == "10");
	REQUIRE(Rows[Order[1]].at("fv") == "10");
	REQUIRE(Rows[Order[2]].at("fv") == "10");
	REQUIRE(Rows[Order[0]].at("lv") == "30");
	REQUIRE(Rows[Order[1]].at("lv") == "30");
	REQUIRE(Rows[Order[2]].at("lv") == "30");
	REQUIRE(Rows[Order[0]].at("nv") == "20");
	REQUIRE(Rows[Order[1]].at("nv") == "20");
	REQUIRE(Rows[Order[2]].at("nv") == "20");
	REQUIRE(Rows[Order[3]].at("fv") == Rows[Order[4]].at("fv"));
	REQUIRE(Rows[Order[3]].at("lv") == "15");
	REQUIRE(Rows[Order[3]].at("nv") == "15");
}

TEST_CASE("SQL: PERCENT_RANK and CUME_DIST on ties") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_wdist_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_wdist.log").string(), false);
	const char *Q =
	    "CREATE TABLE wdist (x INT); INSERT INTO wdist VALUES (10), (20), (20), (40); "
	    "SELECT x, PERCENT_RANK() OVER (ORDER BY x ASC) AS pr, "
	    "CUME_DIST() OVER (ORDER BY x ASC) AS cd FROM wdist;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "wdist");
	REQUIRE(Rows.size() == 4);
	std::vector<size_t> Order(Rows.size());
	std::iota(Order.begin(), Order.end(), size_t{0});
	std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
		const int Xa = std::stoi(Rows[A].at("x"));
		const int Xb = std::stoi(Rows[B].at("x"));
		if(Xa != Xb)
			return Xa < Xb;
		return A < B;
	});
	auto Nearly = [](const std::string &S, double Expected) {
		const double Delta = std::stod(S) - Expected;
		return Delta < 1e-9 && Delta > -1e-9;
	};
	REQUIRE(Nearly(Rows[Order.front()].at("pr"), 0.0));
	REQUIRE(Nearly(Rows[Order.back()].at("pr"), 1.0));
	REQUIRE(Nearly(Rows[Order.back()].at("cd"), 1.0));
	double PrevPr = -1.0;
	double PrevCd = -1.0;
	for(size_t Idx : Order) {
		const double Pr = std::stod(Rows[Idx].at("pr"));
		const double Cd = std::stod(Rows[Idx].at("cd"));
		REQUIRE(Pr >= 0.0);
		REQUIRE(Pr <= 1.0);
		REQUIRE(Cd >= 0.0);
		REQUIRE(Cd <= 1.0);
		REQUIRE(Pr + 1e-9 >= PrevPr);
		REQUIRE(Cd + 1e-9 >= PrevCd);
		PrevPr = Pr;
		PrevCd = Cd;
	}
}

TEST_CASE("SQL: NTILE distributes rows into buckets") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ntile_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_ntile.log").string(), false);
	const char *Q =
	    "CREATE TABLE wnt (x INT); INSERT INTO wnt VALUES (1), (2), (3), (4), (5); "
	    "SELECT x, NTILE(3) OVER (ORDER BY x ASC) AS tile FROM wnt;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "wnt");
	REQUIRE(Rows.size() == 5);
	std::vector<size_t> Order(Rows.size());
	std::iota(Order.begin(), Order.end(), size_t{0});
	std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return Rows[A].at("x") < Rows[B].at("x"); });
	REQUIRE(Rows[Order[0]].at("tile") == "1");
	REQUIRE(Rows[Order[1]].at("tile") == "1");
	REQUIRE(Rows[Order[2]].at("tile") == "2");
	REQUIRE(Rows[Order[3]].at("tile") == "2");
	REQUIRE(Rows[Order[4]].at("tile") == "3");
}

TEST_CASE("SQL: CASE expression in SELECT (searched WHEN/THEN/ELSE)") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_case_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_case.log").string(), false);
	const char *Q =
	    "CREATE TABLE cv (tag INT); "
	    "INSERT INTO cv VALUES (1), (2), (5); "
	    "SELECT tag, CASE WHEN tag < 3 THEN 'lo' WHEN tag IS NULL THEN 'na' ELSE 'hi' END AS bucket FROM cv ORDER BY tag ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "cv");
	REQUIRE(Rows.size() == size_t{3});
	REQUIRE(Rows[0].at("tag") == "1");
	REQUIRE(Rows[0].at("bucket") == "lo");
	REQUIRE(Rows[1].at("tag") == "2");
	REQUIRE(Rows[1].at("bucket") == "lo");
	REQUIRE(Rows[2].at("tag") == "5");
	REQUIRE(Rows[2].at("bucket") == "hi");
}

TEST_CASE("SQL: CASE without ELSE yields null column on no match") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_case_null_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_casen.log").string(), false);
	const char *Q =
	    "CREATE TABLE cx (flag INT); "
	    "INSERT INTO cx VALUES (0), (1); "
	    "SELECT flag, CASE WHEN flag <> 0 THEN 'yes' END AS yn FROM cx ORDER BY flag ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "cx");
	REQUIRE(Rows.size() == size_t{2});
	REQUIRE(Rows[0].at("flag") == "0");
	REQUIRE(Rows[0].find("yn") == Rows[0].end());
	REQUIRE(Rows[1].at("flag") == "1");
	REQUIRE(Rows[1].at("yn") == "yes");
}

TEST_CASE("SQL: CAST in SELECT (integer text and BOOLEAN)") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_cast_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_cast.log").string(), false);
	const char *Q =
	    "CREATE TABLE c1 (sx TEXT); "
	    "INSERT INTO c1 VALUES ('3.7'), ('42'); "
	    "SELECT sx, CAST(sx AS INTEGER) AS ni, CAST(sx AS BOOLEAN) AS nb FROM c1 ORDER BY sx ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "c1");
	REQUIRE(Rows.size() == size_t{2});
	REQUIRE(Rows[0].at("sx") == "3.7");
	REQUIRE(Rows[0].at("ni") == "3");
	REQUIRE(Rows[0].at("nb") == "1");
	REQUIRE(Rows[1].at("sx") == "42");
	REQUIRE(Rows[1].at("ni") == "42");
	REQUIRE(Rows[1].at("nb") == "1");
}

TEST_CASE("SQL: CHECK rejects bad INSERT and UPDATE (column + table)") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_chk_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_chk.log").string(), false);
	const char *Qfail = "CREATE TABLE ck (x INT CHECK ( x > 0 )); INSERT INTO ck VALUES (-5);";
	AstralDB::SQL::Parser P1(Qfail);
	AstralDB::SQL::Bytecode C1 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I1(&Log);
	REQUIRE_THROWS_AS(I1.Execute(C1), std::runtime_error);
	const char *Qok =
	    "CREATE TABLE ck2 (a INT, b INT, CHECK ( a <= b )); INSERT INTO ck2 VALUES (1, 2); "
	    "CREATE TABLE chk_t (id INT CHECK ( id >= 10 )); INSERT INTO chk_t VALUES (42); ";
	AstralDB::SQL::Parser P2(Qok);
	AstralDB::SQL::Bytecode C2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I2(&Log);
	I2.Execute(C2);
	const AstralDB::Database *Db = I2.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "ck2").size() == 1);
	REQUIRE(AllRows(Db, "chk_t").size() == 1);
	REQUIRE(AllRows(Db, "chk_t")[0].at("id") == "42");
}

TEST_CASE("Database: column CHECK enforced on Insert Update API") {
	fs::path Dir = UniqueTempDir("astral_chk_api_");
	fs::path DbPath = Dir / "api.db";
	RemoveEphemeralDb(Dir);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::Database::Schema Sch;
		AstralDB::Database::Column Cx;
		Cx.Name = "x";
		Cx.DefaultValue = "INT";
		std::string Blob;
		AstralDB::SQL::CompileCheckSqlToDnfPackedOrThrow("x < 10", Blob);
		REQUIRE(!Blob.empty());
		{
			AstralDB::Database::Item Probe;
			Probe["x"] = "11";
			REQUIRE_FALSE(AstralDB::SQL::EvaluatePackedWhereDnf(&Db, Probe, Blob));
		}
		Cx.CheckConstraintDnfPacked = std::move(Blob);
		Cx.CheckConstraintSql = std::string("x < 10");
		REQUIRE(Cx.CheckConstraintDnfPacked.has_value());
		REQUIRE(Cx.CheckConstraintSql.has_value());
		Sch.push_back(std::move(Cx));
		REQUIRE(Sch.front().CheckConstraintDnfPacked.has_value());
		REQUIRE(Sch.front().CheckConstraintSql.has_value());
		{
			AstralDB::Database::Schema SchArg = Sch;
			Db.CreateTable("cu", SchArg).wait();
			REQUIRE_MESSAGE(SchArg.front().CheckConstraintDnfPacked.has_value(),
			                "caller schema lost CHECK blob after CreateTable");
			REQUIRE_MESSAGE(SchArg.front().CheckConstraintSql.has_value(),
			                "caller schema lost CHECK SQL after CreateTable");
		}
		{
			auto OptSch = Db.TableSchemaSnapshot("cu");
			REQUIRE(OptSch.has_value());
			REQUIRE(OptSch->size() == 1);
			REQUIRE(OptSch->front().Name == "x");
			REQUIRE(OptSch->front().CheckConstraintSql.has_value());
			REQUIRE_MESSAGE(OptSch->front().CheckConstraintDnfPacked.has_value(),
			                "schema row missing compiled CHECK predicate");
		}
		AstralDB::Database::Item Ok;
		Ok["x"] = "5";
		Db.Insert("cu", Ok).wait();
		AstralDB::Database::Item Bad;
		Bad["x"] = "11";
		REQUIRE_THROWS_AS(Db.Insert("cu", Bad).get(), std::runtime_error);
		AstralDB::Database::Item NewVals;
		NewVals["x"] = "11";
		REQUIRE_THROWS_AS(
		    Db.Update("cu", [](const AstralDB::Database::Item &) { return true; }, NewVals).get(),
		    std::runtime_error);
	}
}

TEST_CASE("SQL: CHECK rejects UPDATE that violates merged row") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_chk_u_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_chku.log").string(), false);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.DatabasePath(fs::absolute(Dir / "astral.db"));
	const char *SetupSql = "CREATE TABLE cu (x INT CHECK ( x < 10 )); INSERT INTO cu VALUES (5);";
	AstralDB::SQL::Parser Ps(SetupSql);
	AstralDB::SQL::Bytecode SetupCode =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	I.Execute(SetupCode);
	const AstralDB::Database *SnapshotDb = I.PrimaryDatabase();
	REQUIRE(SnapshotDb != nullptr);
	REQUIRE(AllRows(SnapshotDb, "cu").size() == 1);
	{
		auto OptSch = SnapshotDb->TableSchemaSnapshot("cu");
		REQUIRE(OptSch.has_value());
		REQUIRE(OptSch->front().CheckConstraintDnfPacked.has_value());
	}
	const char *ViolateSql = "UPDATE cu SET x = 11;";
	AstralDB::SQL::Parser Pu(ViolateSql);
	AstralDB::SQL::Bytecode UpdateCode =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_THROWS_AS(I.Execute(UpdateCode), std::runtime_error);

	fs::path DirOne = UniqueTempDir("astral_chk_u_single_");
	ScopedCwd CwdOne(DirOne);
	RemoveEphemeralDb(DirOne);
	AstralDB::SQL::BytecodeInterpreter Ion(&Log);
	Ion.DatabasePath(fs::absolute(DirOne / "astral.db"));
	const char *AllInOne =
	    "CREATE TABLE cu (x INT CHECK ( x < 10 )); INSERT INTO cu VALUES (5); UPDATE cu SET x = 11;";
	AstralDB::SQL::Parser Pon(AllInOne);
	AstralDB::SQL::Bytecode OneCode =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_THROWS_AS(Ion.Execute(OneCode), std::runtime_error);
}

TEST_CASE("SQL: comma FROM is chained CROSS JOIN") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_cf_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_cf.log").string(), false);
	const char *Q =
	    "CREATE TABLE pf (a INT); CREATE TABLE qf (b INT); INSERT INTO pf VALUES (1),(2),(3); INSERT INTO qf "
	    "VALUES (10),(20); SELECT a, b FROM pf, qf;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "__AstralJoin_0").size() == 6);
}

TEST_CASE("SQL: CAST invalid to INTEGER yields null cell") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_cast_null_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_castn.log").string(), false);
	const char *Q =
	    "CREATE TABLE c2 (sx TEXT); "
	    "INSERT INTO c2 VALUES ('x'), ('1'); "
	    "SELECT sx, CAST(sx AS INTEGER) AS n FROM c2 ORDER BY sx ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "c2");
	REQUIRE(Rows.size() == size_t{2});
	REQUIRE(Rows[1].at("sx") == "x");
	REQUIRE(Rows[1].find("n") == Rows[1].end());
	REQUIRE(Rows[0].at("n") == "1");
	REQUIRE(Rows[0].at("sx") == "1");
}

TEST_CASE("WriteAheadLog: recover multiple inserts after reopen") {
	AstralTest::PerfSection Perf("WAL multi reopen");
	fs::path Dir = UniqueTempDir("astral_wal_m_");
	fs::path DbPath = Dir / "walm.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::Database::Schema Sch;
		AstralDB::Database::Column C;
		C.Name = "n";
		C.DefaultValue = "INT";
		C.IsPrimaryKey = false;
		C.IsUnique = false;
		C.IsNotNull = false;
		Sch.push_back(C);
		Db.CreateTable("rows", Sch).get();
		for(int I = 1; I <= 4; ++I)
			Db.Insert("rows", {{"n", std::to_string(I)}}).get();
		REQUIRE(AllRows(&Db, "rows").size() == 4);
	}
	AstralDB::Database Db2(DbPath, nullptr);
	REQUIRE(TableExists(&Db2, "rows"));
	REQUIRE(AllRows(&Db2, "rows").size() == 4);
}

TEST_CASE("SQL: INNER JOIN materializes expected row count") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ij_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "j.log").string(), false);
	const char *Q = "CREATE TABLE xa (k INT); CREATE TABLE xb (k INT, v TEXT); "
	                "INSERT INTO xa VALUES (1), (2); INSERT INTO xb VALUES (1,'a'), (2,'b'); "
	                "SELECT k, v FROM xa INNER JOIN xb ON xa.k = xb.k;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Jr = AllRows(Db, "__AstralJoin_0");
	REQUIRE(Jr.size() == 2);
	std::set<std::string> Vs;
	Vs.insert(Jr[0].at("v"));
	Vs.insert(Jr[1].at("v"));
	REQUIRE(Vs.count("a") == 1);
	REQUIRE(Vs.count("b") == 1);
}

TEST_CASE("SQL: LEFT JOIN preserves left when no right match") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_lj_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "lj.log").string(), false);
	const char *Q = "CREATE TABLE x (k INT); CREATE TABLE y (k INT, w TEXT); INSERT INTO x VALUES (1),(3); "
	                "INSERT INTO y VALUES (1,'z'); SELECT k, w FROM x LEFT JOIN y ON x.k = y.k ORDER BY k ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Jr = AllRows(Db, "__AstralJoin_0");
	REQUIRE(Jr.size() == 2);
	REQUIRE(Jr[0].at("k") == "1");
	REQUIRE(Jr[0].at("w") == "z");
	REQUIRE(Jr[1].at("k") == "3");
	REQUIRE(Jr[1].at("w").empty());
}

TEST_CASE("SQL: CROSS JOIN is Cartesian product") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_cj_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "cj.log").string(), false);
	const char *Q = "CREATE TABLE p (a INT); CREATE TABLE q (b INT); INSERT INTO p VALUES (1),(2),(3); INSERT INTO q VALUES (10),(20); SELECT a, b FROM p CROSS JOIN q;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "__AstralJoin_0").size() == 6);
}

TEST_CASE("SQL: GROUP BY SUM and COUNT together") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_sum_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "s.log").string(), false);
	const char *Q =
	    "CREATE TABLE sales (region TEXT, amt INT); "
	    "INSERT INTO sales VALUES ('east','10'),('east','20'),('west','5'); "
	    "SELECT region, SUM ( amt ) AS total, COUNT(*) FROM sales GROUP BY region;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "sales");
	REQUIRE(R.size() == 2);
	const AstralDB::Database::Item *EastPtr = nullptr;
	const AstralDB::Database::Item *WestPtr = nullptr;
	for(const auto &Row : R) {
		if(Row.at("region") == "east")
			EastPtr = &Row;
		if(Row.at("region") == "west")
			WestPtr = &Row;
	}
	REQUIRE(EastPtr != nullptr);
	REQUIRE(WestPtr != nullptr);
	const AstralDB::Database::Item &East = *EastPtr;
	const AstralDB::Database::Item &West = *WestPtr;
	REQUIRE(East.at("region") == "east");
	REQUIRE(East.at("cnt") == "2");
	REQUIRE(East.at("total") == "30");
	REQUIRE(West.at("region") == "west");
	REQUIRE(West.at("cnt") == "1");
	REQUIRE(West.at("total") == "5");
}

TEST_CASE("SQL: GROUP BY MIN, MAX, and AVG") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_mma_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "mma.log").string(), false);
	const char *Q = "CREATE TABLE scores (team TEXT, pts INT); "
	                "INSERT INTO scores VALUES ('a','10'),('a','30'),('b','5'); "
	                "SELECT team, MIN(pts) AS lo, MAX(pts) AS hi, AVG(pts) AS mid FROM scores GROUP BY team ORDER BY team ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "scores");
	REQUIRE(R.size() == 2);
	REQUIRE(R[0].at("team") == "a");
	REQUIRE(R[0].at("lo") == "10");
	REQUIRE(R[0].at("hi") == "30");
	REQUIRE(R[0].at("mid") == "20");
	REQUIRE(R[1].at("team") == "b");
	REQUIRE(R[1].at("lo") == "5");
	REQUIRE(R[1].at("hi") == "5");
	REQUIRE(R[1].at("mid") == "5");
}

TEST_CASE("SQL: RIGHT JOIN keeps unmatched right rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_rj_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "rj.log").string(), false);
	const char *Q = "CREATE TABLE rl (k INT); CREATE TABLE rr (k INT, w TEXT); INSERT INTO rl VALUES (1); "
	                "INSERT INTO rr VALUES (1,'m'),(2,'orphan'); "
	                "SELECT k, w FROM rl RIGHT JOIN rr ON rl.k = rr.k ORDER BY k ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Jr = AllRows(Db, "__AstralJoin_0");
	REQUIRE(Jr.size() == 2);
	REQUIRE(Jr[0].at("k") == "1");
	REQUIRE(Jr[0].at("w") == "m");
	REQUIRE(Jr[1].at("k") == "2");
	REQUIRE(Jr[1].at("w") == "orphan");
}

TEST_CASE("SQL: HAVING filters grouped rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_hv_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "hv.log").string(), false);
	const char *Q = "CREATE TABLE hv (g TEXT, v INT); "
	                "INSERT INTO hv VALUES ('p',1),('p',2),('q',3); "
	                "SELECT g, COUNT(*) FROM hv GROUP BY g HAVING cnt > 1 ORDER BY g ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "hv");
	REQUIRE(R.size() == 1);
	REQUIRE(R[0].at("g") == "p");
	REQUIRE(R[0].at("cnt") == "2");
}

TEST_CASE("SQL: COUNT(*) AS alias names grouped column and HAVING") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_hv_alias_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "hv_alias.log").string(), false);
	const char *Q = "CREATE TABLE hv2 (g TEXT, v INT); "
	                "INSERT INTO hv2 VALUES ('p',1),('p',2),('q',3); "
	                "SELECT g, COUNT(*) AS n FROM hv2 GROUP BY g HAVING n > 1 ORDER BY g ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "hv2");
	REQUIRE(R.size() == 1);
	REQUIRE(R[0].at("g") == "p");
	REQUIRE(R[0].at("n") == "2");
}

TEST_CASE("SQL: HAVING COUNT(*) lowered to grouped count column") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_hv_cntcall_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "hv_cntcall.log").string(), false);
	const char *Q = "CREATE TABLE hv3 (g TEXT, v INT); "
	                "INSERT INTO hv3 VALUES ('p',1),('p',2),('q',3); "
	                "SELECT g, COUNT(*) AS order_cnt FROM hv3 GROUP BY g HAVING COUNT(*) > 1 ORDER BY g ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "hv3");
	REQUIRE(R.size() == 1);
	REQUIRE(R[0].at("g") == "p");
	REQUIRE(R[0].at("order_cnt") == "2");
}

TEST_CASE("SQL: HAVING SUM lowered to grouped SUM output column") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_hv_sum_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "hv_sum.log").string(), false);
	const char *Q = "CREATE TABLE s2 (region TEXT, amt INT); "
	                "INSERT INTO s2 VALUES ('east','10'),('east','20'),('west','5'); "
	                "SELECT region, SUM(amt) AS total, COUNT(*) FROM s2 GROUP BY region HAVING SUM(amt) > 15 ORDER BY "
	                "region ASC;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "s2");
	REQUIRE(R.size() == 1);
	REQUIRE(R[0].at("region") == "east");
	REQUIRE(R[0].at("total") == "30");
	REQUIRE(R[0].at("cnt") == "2");
}

TEST_CASE("SQL: WITH CTE uses qualified GROUP BY and HAVING COUNT(*)") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_with_hv_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "with_hv.log").string(), false);
	const char *Q =
	    "CREATE TABLE ctab (id INT); "
	    "INSERT INTO ctab VALUES (1),(1),(1),(1),(1),(1); "
	    "WITH agg AS (SELECT id, COUNT(*) AS cnt FROM ctab GROUP BY ctab.id HAVING COUNT(*) > 5) "
	    "SELECT id, cnt FROM agg;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "__astral_cte_0_agg");
	REQUIRE(R.size() == 1);
	REQUIRE(R[0].at("id") == "1");
	REQUIRE(R[0].at("cnt") == "6");
}

TEST_CASE("SQL: UNION / INTERSECT / EXCEPT compound SELECT") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_setops_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "setops.log").string(), false);
	const char *Q =
	    "CREATE TABLE s1 (n INT); CREATE TABLE s2 (n INT); "
	    "INSERT INTO s1 VALUES (1), (2), (2); "
	    "INSERT INTO s2 VALUES (2), (3); "
	    "SELECT n FROM s1 UNION SELECT n FROM s2 ORDER BY n ASC; "
	    "SELECT n FROM s1 UNION ALL SELECT n FROM s2; "
	    "SELECT n FROM s1 INTERSECT SELECT n FROM s2; "
	    "SELECT n FROM s1 EXCEPT SELECT n FROM s2;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "s1").size() == 3u);
	REQUIRE(AllRows(Db, "s2").size() == 2u);
}

TEST_CASE("SQL: LIMIT with OFFSET skips then caps rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_lo_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "lo.log").string(), false);
	const char *Q = "CREATE TABLE lo (n INT); INSERT INTO lo VALUES (1),(2),(3),(4); "
	                "SELECT n FROM lo ORDER BY n ASC LIMIT 2 OFFSET 1;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE(std::find_if(Code.begin(), Code.end(), [](const AstralDB::SQL::Instruction &In) {
		        return In.Opcode_ == AstralDB::SQL::Opcode::SLICE_RANGE;
	        }) != Code.end());
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	I.Execute(Code);
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto R = AllRows(Db, "lo");
	REQUIRE(R.size() == 2);
	REQUIRE(R[0].at("n") == "2");
	REQUIRE(R[1].at("n") == "3");
}

TEST_CASE("Database: parallel Select futures under shared lock policy") {
	fs::path Dir = UniqueTempDir("astral_parallel_");
	fs::path DbPath = Dir / "pl.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	AstralDB::Database::Schema Sch;
	AstralDB::Database::Column C;
	C.Name = "n";
	C.DefaultValue = "INT";
	Sch.push_back(C);
	Db.CreateTable("t", Sch).get();
	for(int I = 0; I < 20; ++I)
		Db.Insert("t", {{"n", std::to_string(I)}}).get();
	auto F1 = Db.Select("t", [](const AstralDB::Database::Item &) { return true; });
	auto F2 = Db.Select("t", [](const AstralDB::Database::Item &) { return true; });
	REQUIRE(F1.get().size() == 20);
	REQUIRE(F2.get().size() == 20);
}

TEST_CASE("SQL: CREATE VIEW and DROP VIEW catalog") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_sql_viewdrop_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "view.log").string(), false);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astral.db");
	Interp.EnsurePrimaryDatabaseOpened();
	{
		const char *Q =
		    "CREATE TABLE t (id INT); INSERT INTO t VALUES (1); CREATE VIEW vw AS SELECT id FROM t;";
		AstralDB::SQL::Parser P(Q);
		AstralDB::SQL::Bytecode Code =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None, Interp.PrimaryDatabase());
		REQUIRE_NOTHROW(Interp.Execute(Code));
	}
	REQUIRE(Interp.PrimaryDatabase()->HasViewDefinition("vw"));
	{
		const char *Q2 = "DROP VIEW vw;";
		AstralDB::SQL::Parser P2(Q2);
		AstralDB::SQL::Bytecode Code2 =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None, Interp.PrimaryDatabase());
		REQUIRE_NOTHROW(Interp.Execute(Code2));
	}
	REQUIRE_FALSE(Interp.PrimaryDatabase()->HasViewDefinition("vw"));
}

TEST_CASE("SQL: view definitions persist across new connection") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_sql_viewwal_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	const fs::path DbFile = Dir / "astral.db";
	{
		AstralDB::Logger Log((Dir / "w1.log").string(), false);
		AstralDB::SQL::BytecodeInterpreter Interp(&Log);
		Interp.DatabasePath(DbFile);
		Interp.EnsurePrimaryDatabaseOpened();
		const char *Q =
		    "CREATE TABLE t (id INT); INSERT INTO t VALUES (1); CREATE VIEW vw AS SELECT id FROM t;";
		AstralDB::SQL::Parser P(Q);
		AstralDB::SQL::Bytecode Code =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None, Interp.PrimaryDatabase());
		REQUIRE_NOTHROW(Interp.Execute(Code));
		REQUIRE(Interp.PrimaryDatabase()->HasViewDefinition("vw"));
	}
	{
		AstralDB::Logger Log2((Dir / "w2.log").string(), false);
		AstralDB::SQL::BytecodeInterpreter I2(&Log2);
		I2.DatabasePath(DbFile);
		I2.EnsurePrimaryDatabaseOpened();
		REQUIRE(I2.PrimaryDatabase()->HasViewDefinition("vw"));
	}
}

TEST_CASE("Database: catalog users survive authentication and duplicate AddUser fails") {
	fs::path Dir = UniqueTempDir("astral_users_");
	fs::path DbPath = Dir / "u.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	REQUIRE(Db.AuthenticateUser("Admin0", "admin"));
	REQUIRE(Db.IsAuthenticated());
	REQUIRE(Db.CurrentUser().has_value());
	REQUIRE(Db.CurrentUser()->Name == "Admin0");
	REQUIRE_FALSE(Db.AuthenticateUser("Admin0", "wrong"));
	REQUIRE(Db.IsAuthenticated());
	REQUIRE(Db.CurrentUser()->Name == "Admin0");

	AstralDB::User Extra("alice", "alicepw", AstralDB::Permissions::Select);
	REQUIRE_NOTHROW(Db.AddUser(Extra).get());
	REQUIRE(Db.AuthenticateUser("alice", "alicepw"));
	REQUIRE(Db.CurrentUser()->Name == "alice");
	REQUIRE(Db.AuthenticateUser("Admin0", "admin"));
	REQUIRE(Db.CurrentUser()->Name == "Admin0");

	REQUIRE_THROWS_AS(Db.AddUser(Extra).get(), std::runtime_error);
}

TEST_CASE("Database: RemoveUser evicts session and rejects unknown users") {
	fs::path Dir = UniqueTempDir("astral_rmuser_");
	fs::path DbPath = Dir / "rm.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	AstralDB::User Extra("alice", "alicepw", AstralDB::Permissions::Select);
	Db.AddUser(Extra).get();
	REQUIRE(Db.AuthenticateUser("alice", "alicepw"));
	REQUIRE_THROWS_AS(Db.RemoveUser(AstralDB::User("nope", "x", AstralDB::Permissions::Select)).get(), std::runtime_error);
	REQUIRE(Db.IsAuthenticated());
	Db.RemoveUser(Extra).get();
	REQUIRE_FALSE(Db.IsAuthenticated());
	REQUIRE_FALSE(Db.AuthenticateUser("alice", "alicepw"));
	REQUIRE(Db.AuthenticateUser("Admin0", "admin"));
}

TEST_CASE("Database: SetCurrentUser requires catalog membership") {
	fs::path Dir = UniqueTempDir("astral_setuser_");
	fs::path DbPath = Dir / "su.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	REQUIRE_THROWS_AS(Db.SetCurrentUser(AstralDB::User("ghost", "g", AstralDB::Permissions::Select)).get(), std::runtime_error);
	AstralDB::User Extra("alice", "alicepw", AstralDB::Permissions::Select);
	Db.AddUser(Extra).get();
	REQUIRE_NOTHROW(Db.SetCurrentUser(Extra).get());
	REQUIRE(Db.IsAuthenticated());
	REQUIRE(Db.CurrentUser()->Name == "alice");
}

TEST_CASE("Database: ACL enforced when session authenticated") {
	fs::path Dir = UniqueTempDir("astral_acl_enf_");
	fs::path DbPath = Dir / "acl.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	AstralDB::Database::Schema Sch;
	AstralDB::Database::Column C;
	C.Name = "id";
	C.IsPrimaryKey = true;
	C.DefaultValue = "INT";
	Sch.push_back(C);
	Db.CreateTable("orders", Sch).get();
	Db.Insert("orders", {{"id", "1"}}).get();
	AstralDB::User Alice("alice", "secret", AstralDB::Permissions::Select);
	Db.AddUser(Alice).get();
	Db.GrantPermission("alice", AstralDB::Permissions::Select, "orders").get();
	REQUIRE(Db.AuthenticateUser("alice", "secret"));
	REQUIRE_THROWS_AS(Db.Insert("orders", {{"id", "2"}}).get(), std::runtime_error);
	const auto Rows = Db.Select("orders", [](const AstralDB::Database::Item &) { return true; }).get();
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("id") == "1");
}

TEST_CASE("Database: row-level ACL limits visible rows") {
	fs::path Dir = UniqueTempDir("astral_acl_row_");
	fs::path DbPath = Dir / "rowacl.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	AstralDB::Database::Schema Sch;
	AstralDB::Database::Column C;
	C.Name = "id";
	C.IsPrimaryKey = true;
	C.DefaultValue = "INT";
	Sch.push_back(C);
	Db.CreateTable("t", Sch).get();
	Db.Insert("t", {{"id", "1"}}).get();
	Db.Insert("t", {{"id", "2"}}).get();
	AstralDB::User Bob("bob", "pass", AstralDB::Permissions::Select);
	Db.AddUser(Bob).get();
	Db.GrantPermission("bob", AstralDB::Permissions::Select, "t").get();
	AstralDB::RowColPermission Rule;
	Rule.Table = "t";
	Rule.RowId = "2";
	Rule.Column = "";
	Rule.Perms = AstralDB::Permissions::Select;
	Db.GrantRowPermission("bob", Rule).get();
	REQUIRE(Db.AuthenticateUser("bob", "pass"));
	const auto Rows = Db.Select("t", [](const AstralDB::Database::Item &) { return true; }).get();
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("id") == "2");
}

TEST_CASE("Database: RBAC role permissions union with user ACL") {
	fs::path Dir = UniqueTempDir("astral_rbac_");
	fs::path DbPath = Dir / "rbac.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	AstralDB::Database::Schema Sch;
	AstralDB::Database::Column C;
	C.Name = "id";
	C.IsPrimaryKey = true;
	C.DefaultValue = "INT";
	Sch.push_back(C);
	Db.CreateTable("t", Sch).get();
	Db.Insert("t", {{"id", "1"}}).get();
	AstralDB::User Alice("alice", "pw", AstralDB::Permissions::Select);
	Db.AddUser(Alice).get();
	Db.CreateRole("reader").get();
	Db.GrantRolePermission("reader", AstralDB::Permissions::Select, "t").get();
	Db.GrantRoleToUser("reader", "alice").get();
	REQUIRE(Db.AuthenticateUser("alice", "pw"));
	const auto Rows = Db.Select("t", [](const AstralDB::Database::Item &) { return true; }).get();
	REQUIRE(Rows.size() == 1);
}

TEST_CASE("Database: column-level SELECT masks forbidden columns") {
	fs::path Dir = UniqueTempDir("astral_colacl_");
	fs::path DbPath = Dir / "col.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	AstralDB::Database::Schema Sch;
	AstralDB::Database::Column Id;
	Id.Name = "id";
	Id.IsPrimaryKey = true;
	Id.DefaultValue = "INT";
	AstralDB::Database::Column Name;
	Name.Name = "name";
	Name.DefaultValue = "TEXT";
	AstralDB::Database::Column Secret;
	Secret.Name = "secret";
	Secret.DefaultValue = "TEXT";
	Sch.push_back(Id);
	Sch.push_back(Name);
	Sch.push_back(Secret);
	Db.CreateTable("t", Sch).get();
	Db.Insert("t", {{"id", "1"}, {"name", "alice"}, {"secret", "x"}}).get();
	AstralDB::User Carol("carol", "pw", AstralDB::Permissions::Select);
	Db.AddUser(Carol).get();
	Db.GrantPermission("carol", AstralDB::Permissions::Select, "t").get();
	AstralDB::RowColPermission ColRule;
	ColRule.Table = "t";
	ColRule.Column = "name";
	ColRule.Perms = AstralDB::Permissions::Select;
	Db.GrantRowPermission("carol", ColRule).get();
	REQUIRE(Db.AuthenticateUser("carol", "pw"));
	const auto Rows = Db.Select("t", [](const AstralDB::Database::Item &) { return true; }).get();
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].count("name") == 1);
	REQUIRE(Rows[0].at("name") == "alice");
	REQUIRE(Rows[0].count("secret") == 0);
}

TEST_CASE("Database: audit log records login and denial") {
	fs::path Dir = UniqueTempDir("astral_audit_");
	fs::path DbPath = Dir / "audit.db";
	fs::path AuditPath = Dir / "audit.log";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(AuditPath, Ec);
	AstralDB::Database Db(DbPath, nullptr);
	Db.SetAuditLogPath(AuditPath);
	REQUIRE_FALSE(Db.AuthenticateUser("Admin0", "wrong"));
	REQUIRE(Db.AuthenticateUser("Admin0", "admin"));
	Db.Logout();
	REQUIRE(std::filesystem::exists(AuditPath));
	const std::string Log = ReadExampleFile(AuditPath);
	REQUIRE(Log.find("LOGIN") != std::string::npos);
	REQUIRE(Log.find("DENIED") != std::string::npos);
}

TEST_CASE("Database: fine grants persist across snapshot reopen") {
	fs::path Dir = UniqueTempDir("astral_fine_snap_");
	fs::path DbPath = Dir / "fine.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::Database::Schema Sch;
		AstralDB::Database::Column C;
		C.Name = "id";
		C.IsPrimaryKey = true;
		C.DefaultValue = "INT";
		Sch.push_back(C);
		Db.CreateTable("t", Sch).get();
		Db.Insert("t", {{"id", "1"}}).get();
		Db.Insert("t", {{"id", "2"}}).get();
		AstralDB::User U("u", "pw", AstralDB::Permissions::Select);
		Db.AddUser(U).get();
		Db.GrantPermission("u", AstralDB::Permissions::Select, "t").get();
		AstralDB::RowColPermission Rule;
		Rule.Table = "t";
		Rule.RowId = "1";
		Rule.Perms = AstralDB::Permissions::Select;
		Db.GrantRowPermission("u", Rule).get();
		Db.SyncToFile();
	}
	{
		AstralDB::Database Db2(DbPath, nullptr);
		REQUIRE(Db2.AuthenticateUser("u", "pw"));
		const auto Rows = Db2.Select("t", [](const AstralDB::Database::Item &) { return true; }).get();
		REQUIRE(Rows.size() == 1);
		REQUIRE(Rows[0].at("id") == "1");
	}
}

TEST_CASE("Database: users and ACLs persist across snapshot reopen") {
	fs::path Dir = UniqueTempDir("astral_snapusers_");
	fs::path DbPath = Dir / "snap.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::User Alice("alice", "secret", AstralDB::Permissions::Select);
		Db.AddUser(Alice).get();
		Db.GrantPermission("alice", AstralDB::Permissions::Insert, "orders").get();
		Db.SyncToFile();
	}
	{
		AstralDB::Database Db2(DbPath, nullptr);
		REQUIRE(Db2.AuthenticateUser("alice", "secret"));
		REQUIRE(Db2.CurrentUser()->Name == "alice");
		REQUIRE(Db2.HasPermission(*Db2.CurrentUser(), AstralDB::Permissions::Insert, "orders"));
		REQUIRE(Db2.AuthenticateUser("Admin0", "admin"));
	}
}

TEST_CASE("SQL: FOREIGN KEY ON DELETE CASCADE removes dependent rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_fk_cascade_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "fk_cascade.log").string(), false);
	const char *Q =
	    "CREATE TABLE par_c (id INT PRIMARY KEY); "
	    "CREATE TABLE chi_c (pid INT NOT NULL, FOREIGN KEY (pid) REFERENCES par_c(id) ON DELETE CASCADE); "
	    "INSERT INTO par_c VALUES (1); INSERT INTO chi_c VALUES (1); DELETE FROM par_c WHERE id = 1;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "chi_c").empty());
}

TEST_CASE("SQL: FOREIGN KEY ON DELETE SET NULL clears child pointer") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_fk_setnull_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "fk_sn.log").string(), false);
	const char *Q =
	    "CREATE TABLE par_sn (id INT PRIMARY KEY); "
	    "CREATE TABLE chi_sn (pid INT, FOREIGN KEY (pid) REFERENCES par_sn(id) ON DELETE SET NULL); "
	    "INSERT INTO par_sn VALUES (9); INSERT INTO chi_sn VALUES (9); DELETE FROM par_sn WHERE id = 9;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "chi_sn");
	REQUIRE(Rows.size() == 1);
	const auto PidIt = Rows[0].find("pid");
	const bool PidIsNullish = (PidIt == Rows[0].end()) || PidIt->second.empty();
	REQUIRE(PidIsNullish);
}

TEST_CASE("SQL: FOREIGN KEY RESTRICT blocks delete and bad INSERT fails") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_fk_restrict_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "fk_r.log").string(), false);
	{
		const char *Ok =
		    "CREATE TABLE par_r (id INT PRIMARY KEY); "
		    "CREATE TABLE chi_r (pid INT, FOREIGN KEY (pid) REFERENCES par_r(id) ON DELETE RESTRICT); "
		    "INSERT INTO par_r VALUES (1); INSERT INTO chi_r VALUES (1);";
		AstralDB::SQL::Parser P1(Ok);
		AstralDB::SQL::Bytecode C1 =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		AstralDB::SQL::BytecodeInterpreter I1(&Log);
		REQUIRE_NOTHROW(I1.Execute(C1));
		AstralDB::SQL::Parser Pdel("DELETE FROM par_r WHERE id = 1;");
		AstralDB::SQL::Bytecode Cdel =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		REQUIRE_THROWS_AS(I1.Execute(Cdel), std::runtime_error);
	}
	RemoveEphemeralDb(Dir);
	{
		const char *BadIns =
		    "CREATE TABLE par2 (id INT PRIMARY KEY); "
		    "CREATE TABLE chi2 (pid INT, FOREIGN KEY (pid) REFERENCES par2(id)); "
		    "INSERT INTO chi2 VALUES (99);";
		AstralDB::SQL::Parser P2(BadIns);
		AstralDB::SQL::Bytecode C2 =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		AstralDB::SQL::BytecodeInterpreter I2(&Log);
		REQUIRE_THROWS_AS(I2.Execute(C2), std::runtime_error);
	}
}

TEST_CASE("SQL: composite FOREIGN KEY insert succeeds when tuple matches parent PK") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_fk_comp_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "fk_comp.log").string(), false);
	const char *Q =
	    "CREATE TABLE pcombo (a INT, b INT, CONSTRAINT pk_combo PRIMARY KEY (a, b)); "
	    "CREATE TABLE ccombo (ca INT, cb INT, CONSTRAINT fk_combo FOREIGN KEY (ca, cb) REFERENCES pcombo (a, b)); "
	    "INSERT INTO pcombo VALUES (1, 2); INSERT INTO ccombo VALUES (1, 2);";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "ccombo").size() == 1);
	REQUIRE(AllRows(Db, "ccombo")[0].at("ca") == "1");
	REQUIRE(AllRows(Db, "ccombo")[0].at("cb") == "2");
}

TEST_CASE("SQL: FETCH FIRST limits ordered rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_fetch_first_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ff.log").string(), false);
	const char *Q =
	    "CREATE TABLE ff_t (n INT); INSERT INTO ff_t VALUES (10), (30), (20); "
	    "SELECT n FROM ff_t ORDER BY n DESC FETCH FIRST 2 ROWS ONLY;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "ff_t");
	REQUIRE(Rows.size() == 2);
	REQUIRE(Rows[0].at("n") == "30");
	REQUIRE(Rows[1].at("n") == "20");
}

TEST_CASE("SQL: ALTER TABLE RENAME COLUMN moves stored cells") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_rename_col_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "rn.log").string(), false);
	const char *Q =
	    "CREATE TABLE rn_t (old_nm TEXT); INSERT INTO rn_t VALUES ('keep'); ALTER TABLE rn_t RENAME COLUMN old_nm TO "
	    "new_nm;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "rn_t");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("new_nm") == "keep");
}

TEST_CASE("SQL: DROP TABLE CASCADE removes referencing tables") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_drop_cascade_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "dc.log").string(), false);
	const char *Q =
	    "CREATE TABLE dp (id INT PRIMARY KEY); "
	    "CREATE TABLE dc (pid INT, FOREIGN KEY (pid) REFERENCES dp(id)); "
	    "DROP TABLE dp CASCADE;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE_FALSE(TableExists(Db, "dp"));
	REQUIRE_FALSE(TableExists(Db, "dc"));
}

TEST_CASE("SQL: TIME_BUCKET scalar and grouped SUM on bucket column") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ts_bucket_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "tb.log").string(), false);
	const char *Q =
	    "CREATE TABLE ts_m (ts TEXT, v INT); "
	    "INSERT INTO ts_m VALUES ('2024-06-01T12:34:56', 10), ('2024-06-01T12:59:00', 20); "
	    "SELECT TIME_BUCKET(ts, 3600) AS bucket FROM ts_m; "
	    "CREATE TABLE ts_g (bucket TEXT, v INT); "
	    "INSERT INTO ts_g VALUES ('2024-06-01T12:00:00', 10), ('2024-06-01T12:00:00', 20); "
	    "SELECT bucket, SUM(v) AS s FROM ts_g GROUP BY bucket;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Scalar = AllRows(Db, "ts_m");
	REQUIRE(Scalar.size() == 2);
	REQUIRE(Scalar[0].at("bucket").find("2024-06-01T12:00:00") == 0);
	const auto Grouped = AllRows(Db, "ts_g");
	REQUIRE(Grouped.size() == 1);
	REQUIRE(Grouped[0].at("s") == "30");
}

TEST_CASE("SQL: EXTRACT HOUR and TIMESTAMP_DIFF") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ts_diff_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "td.log").string(), false);
	const char *Q =
	    "CREATE TABLE ts_d (ts TEXT); "
	    "INSERT INTO ts_d VALUES ('2024-01-01T03:30:00'); "
	    "SELECT EXTRACT(HOUR FROM ts) AS hr FROM ts_d;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "ts_d");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("hr") == "3");
}

TEST_CASE("SQL: TIMESTAMP_DIFF seconds") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ts_diff2_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "td2.log").string(), false);
	const char *Q =
	    "CREATE TABLE ts_d2 (ts TEXT); "
	    "INSERT INTO ts_d2 VALUES ('2024-01-01T03:30:00'); "
	    "SELECT TIMESTAMP_DIFF('2024-01-01T03:00:00', ts) AS d FROM ts_d2;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "ts_d2");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("d") == "1800");
}

TEST_CASE("SQL: EXTRACT and DATE_ADD on ISO date text") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_extract_date_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ed.log").string(), false);
	const char *Q =
	    "CREATE TABLE dt_row (d TEXT); INSERT INTO dt_row VALUES ('2020-06-15'); "
	    "SELECT EXTRACT(YEAR FROM d) AS y, DATE_ADD(d, 10) AS d2 FROM dt_row;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "dt_row");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("y") == "2020");
	REQUIRE(!Rows[0].at("d2").empty());
}

TEST_CASE("SQL: CREATE USER DROP USER and ALTER USER password") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_sqluser_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "sqluser.log").string(), false);
	const char *Q =
	    "CREATE USER app1 IDENTIFIED BY 'pw1'; "
	    "CREATE USER IF NOT EXISTS app1 IDENTIFIED BY 'ignored'; "
	    "ALTER USER app1 IDENTIFIED BY 'pw2'; "
	    "CREATE TABLE u_t (id INT); "
	    "GRANT SELECT ON u_t TO app1;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(Db->AuthenticateUser("app1", "pw2"));
	REQUIRE_FALSE(Db->AuthenticateUser("app1", "pw1"));
	Db->Logout();
	REQUIRE(Db->AuthenticateUser("Admin0", "admin"));
	const char *DropQ = "DROP USER app1;";
	AstralDB::SQL::Parser P2(DropQ);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(I.Execute(Code2));
	REQUIRE_FALSE(Db->AuthenticateUser("app1", "pw2"));
}

TEST_CASE("SQL: GRANT WITH GRANT OPTION executes") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_grant_opt_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "go.log").string(), false);
	const char *Q =
	    "CREATE TABLE go_t (id INT); GRANT SELECT ON go_t TO Admin0 WITH GRANT OPTION;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
}

TEST_CASE("SQL: VARCHAR(n) does not truncate inserted text (length hint only)") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_varchar_len_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "vc.log").string(), false);
	const char *Long = "abcdefghijklmnopqrstuvwxyz";
	const char *Q =
	    "CREATE TABLE vc_t (s VARCHAR(4)); "
	    "INSERT INTO vc_t VALUES ('abcdefghijklmnopqrstuvwxyz');";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "vc_t")[0].at("s") == Long);
}

TEST_CASE("SQL: MERGE and UPSERT parse and run") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_merge_upsert_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "mu.log").string(), false);
	const char *Q =
	    "CREATE TABLE tgt (id INT PRIMARY KEY, v TEXT); "
	    "CREATE TABLE src (sk INT, v TEXT); "
	    "INSERT INTO tgt VALUES (1, 'a'); "
	    "INSERT INTO src VALUES (1, 'b'), (2, 'c'); "
	    "MERGE INTO tgt AS t USING src AS s ON t.id = s.sk "
	    "WHEN MATCHED THEN UPDATE SET v = s.v "
	    "WHEN NOT MATCHED THEN INSERT (id, v) VALUES (s.sk, s.v); "
	    "INSERT INTO tgt (id, v) VALUES (3, 'z'); "
	    "INSERT INTO tgt (id, v) VALUES (3, 'nope') ON CONFLICT (id) DO UPDATE SET v = 'u'; "
	    "INSERT INTO tgt (id, v) VALUES (3, 'w') ON CONFLICT DO NOTHING;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto T = AllRows(Db, "tgt");
	REQUIRE(T.size() == size_t(3));
	// id=1 updated from src, id=2 inserted
	std::unordered_map<std::string, std::string> ById;
	for(const auto &R : T)
		ById[R.at("id")] = R.at("v");
	REQUIRE(ById["1"] == "b");
	REQUIRE(ById["2"] == "c");
	REQUIRE(ById["3"] == "u");
}

TEST_CASE("SQL: UPSERT EXCLUDED and MERGE insert-only branch") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_upsert_excluded_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ue.log").string(), false);
	const char *Q =
	    "CREATE TABLE ex_t (id INT PRIMARY KEY, a TEXT, b TEXT); "
	    "INSERT INTO ex_t VALUES (1, 'x', 'y'); "
	    "INSERT INTO ex_t (id, a, b) VALUES (1, 'p', 'q') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = "
	    "'fixed'; "
	    "CREATE TABLE m_only (id INT PRIMARY KEY, v TEXT); "
	    "CREATE TABLE m_src (sk INT, v TEXT); "
	    "INSERT INTO m_src VALUES (9, 'nine'); "
	    "MERGE INTO m_only AS t USING m_src AS s ON t.id = s.sk "
	    "WHEN NOT MATCHED THEN INSERT (id, v) VALUES (s.sk, s.v);";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "ex_t").size() == size_t(1));
	REQUIRE(AllRows(Db, "ex_t")[0].at("a") == "p");
	REQUIRE(AllRows(Db, "ex_t")[0].at("b") == "fixed");
	REQUIRE(AllRows(Db, "m_only").size() == size_t(1));
	REQUIRE(AllRows(Db, "m_only")[0].at("id") == "9");
}

TEST_CASE("SQL: GROUP BY WITH ROLLUP produces subtotal rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_rollup_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ru.log").string(), false);
	const char *Q =
	    "CREATE TABLE ru (k TEXT, n INT); "
	    "INSERT INTO ru VALUES ('a', 1), ('a', 2), ('b', 3); "
	    "SELECT k, cnt FROM ru GROUP BY k WITH ROLLUP;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "ru");
	REQUIRE(Rows.size() == size_t(3));
	std::unordered_map<std::string, std::string> Grand;
	for(const auto &R : Rows) {
		if(R.at("_olap_level") == "0")
			Grand = R;
	}
	REQUIRE(Grand.at("k").empty());
	REQUIRE(Grand.at("cnt") == "3");
}

TEST_CASE("SQL: GROUPING SETS and GROUPING()") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_gs_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "gs.log").string(), false);
	const char *Q =
	    "CREATE TABLE gs (region TEXT, product TEXT, amt INT); "
	    "INSERT INTO gs VALUES ('east', 'a', 10), ('east', 'b', 20), ('west', 'a', 30); "
	    "SELECT region, product, COUNT(*) AS cnt, GROUPING(region) AS gr FROM gs "
	    "GROUP BY region, product GROUPING SETS ((region, product), (region), ());";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "gs");
	REQUIRE(Rows.size() == size_t(6));
}

TEST_CASE("SQL: MERGE composite ON and expression SET") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_mexpr_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "mx.log").string(), false);
	const char *Q =
	    "CREATE TABLE mt (id INT PRIMARY KEY, a INT, b INT); "
	    "CREATE TABLE ms (sk INT, x INT, y INT); "
	    "INSERT INTO mt VALUES (1, 10, 1); "
	    "INSERT INTO ms VALUES (1, 5, 1); "
	    "MERGE INTO mt AS t USING ms AS s ON t.id = s.sk AND t.b = s.y "
	    "WHEN MATCHED THEN UPDATE SET a = t.a + s.x; "
	    "UPDATE mt SET b = b + 1 WHERE id = 1;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "mt")[0].at("a") == "15");
	REQUIRE(AllRows(Db, "mt")[0].at("b") == "2");
}

TEST_CASE("SQL: CREATE SEQUENCE and GENERATED AS IDENTITY") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_seq_id_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "si.log").string(), false);
	const char *Q =
	    "CREATE SEQUENCE s1 START WITH 10 INCREMENT BY 2; "
	    "CREATE TABLE t1 (id INT GENERATED ALWAYS AS IDENTITY, k TEXT PRIMARY KEY); "
	    "INSERT INTO t1 (k) VALUES ('a'); "
	    "INSERT INTO t1 (k) VALUES ('b'); "
	    "CREATE TABLE t2 (x INT PRIMARY KEY); "
	    "INSERT INTO t2 VALUES (NEXTVAL(s1)); "
	    "INSERT INTO t2 VALUES (NEXTVAL(s1)); ";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows1 = AllRows(Db, "t1");
	REQUIRE(Rows1.size() == 2);
	REQUIRE(Rows1[0].at("id") == "1");
	REQUIRE(Rows1[1].at("id") == "2");
	const auto Rows2 = AllRows(Db, "t2");
	REQUIRE(Rows2.size() == 2);
	REQUIRE(Rows2[0].at("x") == "10");
	REQUIRE(Rows2[1].at("x") == "12");
}

TEST_CASE("SQL: INSERT into nonexistent table fails at execute") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ins_miss_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "im.log").string(), false);
	AstralDB::SQL::Parser P("INSERT INTO __astral_absent_relation_z VALUES (1);");
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_THROWS_AS(I.Execute(Code), std::runtime_error);
}

TEST_CASE("SQL: CREATE PROCEDURE caches .abc and CALL runs") {
	AstralTest::PerfSection Perf("CREATE PROCEDURE + CALL");
	fs::path Dir = UniqueTempDir("astral_proc_sql_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "proc.log").string(), false);
	const std::string Sql =
	    "CREATE PROCEDURE p1 AS ( CREATE TABLE pt (x INTEGER); INSERT INTO pt VALUES (7); ); CALL p1;";
	AstralDB::SQL::Parser P(Sql);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astral.db");
	REQUIRE_NOTHROW(Interp.Execute(Code));
	const auto Cat = AstralDB::SQL::LoadProcedureCatalog(AstralDB::SQL::DefaultProcedureCatalogPath(Dir / "astral.db"));
	REQUIRE(AstralDB::SQL::FindProcedure(Cat, "p1").has_value());
}

TEST_CASE("ProcedureParser: PL/pgSQL dollar body is identified and lowered") {
	const char *Src =
	    "CREATE OR REPLACE PROCEDURE pgsql_demo() LANGUAGE plpgsql AS $$ "
	    "BEGIN CREATE TABLE pg_demo (n INTEGER); INSERT INTO pg_demo VALUES (1); END; $$";
	REQUIRE(AstralDB::SQL::ProcedureParser::IsDialectProcedureStatement(Src));
	const auto R = AstralDB::SQL::ProcedureParser(Src).ParseDialectCreate();
	REQUIRE(R.DialectTag == "plpgsql");
	REQUIRE(R.ProcedureName == "pgsql_demo");
	REQUIRE(R.LoweredBodySql.find("CREATE TABLE pg_demo") != std::string::npos);
	REQUIRE(R.LoweredBodySql.find("INSERT INTO pg_demo") != std::string::npos);
}

TEST_CASE("ProcedureParser: PL/SQL IS BEGIN form is identified and lowered") {
	const char *Src = "CREATE OR REPLACE PROCEDURE ora_demo IS BEGIN "
	                  "CREATE TABLE ora_demo (n INTEGER); INSERT INTO ora_demo VALUES (2); END ora_demo;";
	REQUIRE(AstralDB::SQL::ProcedureParser::IsDialectProcedureStatement(Src));
	const auto R = AstralDB::SQL::ProcedureParser(Src).ParseDialectCreate();
	REQUIRE(R.DialectTag == "plsql");
	REQUIRE(R.ProcedureName == "ora_demo");
	REQUIRE(R.OrReplace);
	REQUIRE(R.LoweredBodySql.find("INSERT INTO ora_demo") != std::string::npos);
}

TEST_CASE("SQL: EXEC and EXECUTE PROCEDURE alias CALL") {
	AstralDB::SQL::SetParserDiagnostics(false);
	AstralDB::SQL::Parser P1("EXEC myproc;");
	AstralDB::SQL::Parser P2("EXECUTE PROCEDURE myproc;");
	(void)P1;
	(void)P2;
	const auto Calls = AstralDB::SQL::ScanProcedureCallsInSql("EXEC a; EXECUTE b; EXECUTE PROCEDURE c; CALL d;");
	auto Has = [&](const char *N) {
		return std::find(Calls.begin(), Calls.end(), N) != Calls.end();
	};
	REQUIRE(Has("a"));
	REQUIRE(Has("b"));
	REQUIRE(Has("c"));
	REQUIRE(Has("d"));
}

TEST_CASE("ProcedureParser: EXCEPTION WHEN OTHERS lowers handlers") {
	const char *Src = "CREATE OR REPLACE PROCEDURE ex_demo IS BEGIN "
	                  "INSERT INTO t VALUES (1); "
	                  "EXCEPTION WHEN OTHERS THEN INSERT INTO err VALUES (1); END;";
	const auto R = AstralDB::SQL::ProcedureParser(Src).ParseDialectCreate();
	REQUIRE(R.Body_.ExceptionHandlers.size() == 1);
	REQUIRE(R.Body_.ExceptionHandlers[0].Condition == "OTHERS");
}

TEST_CASE("SQL: PL/pgSQL CREATE PROCEDURE lowers, caches .abc, and CALL runs") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_proc_plpgsql_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "plpgsql.log").string(), false);
	const std::string Sql =
	    "CREATE OR REPLACE PROCEDURE p2() LANGUAGE plpgsql AS $$ "
	    "BEGIN CREATE TABLE pt2 (x INTEGER); INSERT INTO pt2 VALUES (8); END; $$; "
	    "CALL p2;";
	AstralDB::SQL::Parser P(Sql);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astral.db");
	REQUIRE_NOTHROW(Interp.Execute(Code));
	const auto Cat = AstralDB::SQL::LoadProcedureCatalog(AstralDB::SQL::DefaultProcedureCatalogPath(Dir / "astral.db"));
	const auto Entry = AstralDB::SQL::FindProcedure(Cat, "p2");
	REQUIRE(Entry.has_value());
	REQUIRE(Entry->SourceDialect == "plpgsql");
}

TEST_CASE("Triggers: storage-backed create, fire, disable, drop") {
	AstralTest::PerfSection Perf("triggers");
	fs::path Dir = UniqueTempDir("astral_trig_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "trig.log").string(), false);
	const std::string Sql =
	    "CREATE TABLE src (id INTEGER); "
	    "CREATE TABLE trig_log (id INTEGER); "
	    "CREATE TRIGGER bump_after_insert AFTER INSERT ON src FOR EACH ROW "
	    "AS (INSERT INTO trig_log VALUES (1)); "
	    "INSERT INTO src VALUES (1); "
	    "INSERT INTO src VALUES (2);";
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astral.db");
	{
		AstralDB::SQL::Parser P(Sql);
		AstralDB::SQL::Bytecode Code =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		REQUIRE_NOTHROW(Interp.Execute(Code));
		if(AstralDB::Database *DbSync = Interp.PrimaryDatabase())
			DbSync->SyncToFile();
	}
	{
		AstralDB::Database *Db = Interp.PrimaryDatabase();
		REQUIRE(Db != nullptr);
		REQUIRE(Db->TriggerFireDepth() == 0);
		REQUIRE(Db->Tables_.at("src").RowStore.size() == 2);
		REQUIRE(Db->Tables_.at("trig_log").RowStore.size() >= 1);
	}
	const auto Cat0 = AstralDB::SQL::LoadTriggerCatalog(AstralDB::SQL::DefaultTriggerCatalogPath(Dir / "astral.db"));
	REQUIRE(AstralDB::SQL::FindTrigger(Cat0, "bump_after_insert").has_value());
	REQUIRE(AstralDB::SQL::FindTrigger(Cat0, "bump_after_insert")->Enabled);
	const std::string DisableSql = "ALTER TRIGGER bump_after_insert DISABLE; INSERT INTO src VALUES (3);";
	AstralDB::SQL::Parser P2(DisableSql);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(Interp.Execute(Code2));
	{
		AstralDB::Database *Db = Interp.PrimaryDatabase();
		REQUIRE(Db->Tables_.at("trig_log").RowStore.size() >= 1);
	}
	const auto Cat1 = AstralDB::SQL::LoadTriggerCatalog(AstralDB::SQL::DefaultTriggerCatalogPath(Dir / "astral.db"));
	REQUIRE(!AstralDB::SQL::FindTrigger(Cat1, "bump_after_insert")->Enabled);
	const std::string DropSql = "DROP TRIGGER bump_after_insert; INSERT INTO src VALUES (4);";
	AstralDB::SQL::Parser P3(DropSql);
	AstralDB::SQL::Bytecode Code3 =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_NOTHROW(Interp.Execute(Code3));
	{
		AstralDB::Database *Db = Interp.PrimaryDatabase();
		REQUIRE(Db->Tables_.at("trig_log").RowStore.size() == 2);
	}
	const auto Cat2 = AstralDB::SQL::LoadTriggerCatalog(AstralDB::SQL::DefaultTriggerCatalogPath(Dir / "astral.db"));
	REQUIRE(!AstralDB::SQL::FindTrigger(Cat2, "bump_after_insert").has_value());
}

TEST_CASE("Hybrid storage: CREATE USING STORAGE and ALTER SET STORAGE") {
	AstralTest::PerfSection Perf("hybrid storage SQL");
	fs::path Dir = UniqueTempDir("astral_hybrid_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "hybrid.log").string(), false);
	const std::string Sql =
	    "CREATE TABLE t (k TEXT, v INT) USING STORAGE AUTO; "
	    "INSERT INTO t VALUES ('a', 1), ('b', 2); "
	    "ALTER TABLE t SET STORAGE COLUMNAR; "
	    "SELECT /*+ STORAGE(COLUMNAR) */ k, SUM(v) AS s FROM t GROUP BY k;";
	AstralDB::SQL::Parser P(Sql);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astral.db");
	REQUIRE_NOTHROW(Interp.Execute(Code));
}

TEST_CASE("Hybrid storage: policy API and columnar replica") {
	AstralTest::PerfSection Perf("hybrid storage API");
	fs::path Dir = UniqueTempDir("astral_hybrid_api_");
	AstralDB::Logger Log((Dir / "hybrid_api.log").string(), false);
	AstralDB::Database Db(Dir / "hybrid_api.db", &Log);
	AstralDB::Database::Schema Sch;
	{
		AstralDB::Database::Column Ck;
		Ck.Name = "k";
		Ck.DefaultValue = "TEXT";
		Sch.push_back(Ck);
	}
	{
		AstralDB::Database::Column Cv;
		Cv.Name = "v";
		Cv.DefaultValue = "INT";
		Sch.push_back(Cv);
	}
	Db.CreateTable("metrics", Sch, AstralDB::StorageLayout::Auto).get();
	Db.SetTableStoragePolicy("metrics", AstralDB::StorageLayout::Columnar);
	REQUIRE(Db.TableStoragePolicy("metrics") == AstralDB::StorageLayout::Columnar);
	AstralDB::Database::Item Row{{"k", "east"}, {"v", "10"}};
	Db.Insert("metrics", Row).get();
	REQUIRE(Db.Tables_.at("metrics").ColumnarSynced);
	REQUIRE(Db.Tables_.at("metrics").Columnar.RowCount == 1);
}

TEST_CASE("Hybrid storage: MLP scheduler stays under 5KB") {
	AstralDB::HybridStorageScheduler Sched;
	const std::string Blob = Sched.SerializeWeights();
	REQUIRE(Blob.size() < 5120);
	AstralDB::TableWorkloadCounters C;
	C.ReadCount = 100;
	C.WriteCount = 10;
	C.AggregateReadCount = 80;
	C.RowCountPeak = 5000;
	const AstralDB::WorkloadFeatures F = Sched.BuildFeatures(C);
	const AstralDB::StorageLayout L = Sched.PredictLayout(F);
	REQUIRE((L == AstralDB::StorageLayout::Row || L == AstralDB::StorageLayout::Columnar ||
	         L == AstralDB::StorageLayout::Hybrid));
}

TEST_CASE("Bytecode: abc round-trip, inspect, and procedure catalog") {
	AstralDB::Logger Log("bc_tools.log", false);
	AstralDB::SQL::Parser P("CREATE TABLE bc_t (id INTEGER); INSERT INTO bc_t VALUES (1);");
	AstralDB::SQL::CompiledBytecode Compiled =
	    AstralDB::SQL::BuildCompiledBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	const fs::path Abc = UniqueTempDir("astral_bc_") / "test.abc";
	AstralDB::SQL::SaveAbcFile(Abc, Compiled);
	const auto Loaded = AstralDB::SQL::LoadAbcFile(Abc);
	REQUIRE(Loaded.Instructions.size() == Compiled.Instructions.size());
	const auto Analysis = AstralDB::SQL::AnalyzeBytecode(Loaded.Instructions);
	REQUIRE(Analysis.HasDdl);
	REQUIRE(Analysis.HasDml);
	REQUIRE(!Analysis.ReferencedTables.empty());
	const auto Aspect = AstralDB::SQL::QueryBytecodeAspect(Loaded.Instructions, "tables");
	REQUIRE(Aspect.find("bc_t") != std::string::npos);
	const auto Report = AstralDB::SQL::ValidateBytecode(Loaded.Instructions);
	REQUIRE(Report.Ok);
	auto Catalog = AstralDB::SQL::LoadProcedureCatalog(Abc.parent_path() / "astraldb_procs.json");
	Catalog.CatalogPath = Abc.parent_path() / "astraldb_procs.json";
	AstralDB::SQL::RegisterProcedure(Catalog, "seed", Abc, "test proc");
	AstralDB::SQL::SaveProcedureCatalog(Catalog);
	const auto Reloaded = AstralDB::SQL::LoadProcedureCatalog(Catalog.CatalogPath);
	REQUIRE(AstralDB::SQL::FindProcedure(Reloaded, "seed").has_value());
}

TEST_CASE("AdvancedTypes: STRUCT MAP VECTOR MATRIX COMPLEX wire format") {
	const auto Struct = AstralDB::AdvancedTypes::ParseStructCell("S{x=1,y=2}");
	REQUIRE(Struct.has_value());
	REQUIRE(Struct->at("x") == "1");
	const auto Map = AstralDB::AdvancedTypes::ParseMapCell("M{a:1,b:2}");
	REQUIRE(Map.has_value());
	REQUIRE(Map->at("a") == "1");
	const auto Vec = AstralDB::AdvancedTypes::ParseVectorCell("V[3]:1,2,3", 3);
	REQUIRE(Vec.has_value());
	REQUIRE(Vec->size() == 3);
	const auto Mat = AstralDB::AdvancedTypes::DecodeMatrixCell("T[2,2]:1,0,0,1");
	REQUIRE(Mat.has_value());
	REQUIRE(Mat->Rows == 2);
	REQUIRE(Mat->Flat.size() == 4);
	const auto Cx = AstralDB::AdvancedTypes::ParseComplexCell("C(3,4)");
	REQUIRE(Cx.has_value());
	REQUIRE(Cx->first == 3.0);
}

TEST_CASE("SQL: advanced types and SIMD scalar builtins") {
	const auto Pa = AstralDB::MathSciComplex::ParseNumericVec("V[2]:3,4");
	const auto Pb = AstralDB::MathSciComplex::ParseNumericVec("V[2]:1,0");
	REQUIRE(Pa.has_value());
	REQUIRE(Pb.has_value());
	REQUIRE_FALSE(Pa->IsComplex());
	REQUIRE_FALSE(Pb->IsComplex());
	const auto DotCell = AstralDB::MathSciComplex::DotCellFromReal("V[2]:3,4", "V[2]:1,0");
	REQUIRE(DotCell.has_value());
	REQUIRE(std::stod(*DotCell) == AstralTest::Approx(3.0).epsilon(0.001));
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_adv_types_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "at.log").string(), false);
	const char *Q =
	    "CREATE TABLE adv (id INT, v VECTOR(2) FLOAT, c COMPLEX); "
	    "INSERT INTO adv VALUES (1, 'V[2]:3,4', 'C(1,1)'); "
	    "SELECT VECTOR_DOT(v, 'V[2]:1,0') AS dot, COMPLEX_MUL(c, 'C(0,1)') AS cm FROM adv;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	Db->WithExclusiveBytecodeLock([&]() {
		const auto &Row = Db->Tables_.at("adv").RowStore[0];
		REQUIRE(std::stod(Row.at("dot")) == AstralTest::Approx(3.0).epsilon(0.001));
		REQUIRE(Row.at("cm").find("C(") == 0);
	});
}

TEST_CASE("MathSci: elementary and LIST builtins") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_math_sci_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ms.log").string(), false);
	const char *Q =
	    "CREATE TABLE m (id INT, xs LIST(DOUBLE)); "
	    "INSERT INTO m VALUES (1, 'L[3]:1,2,3'); "
	    "SELECT SQRT(9) AS r, MEAN(xs) AS mu, LIST_APPEND(xs, '4') AS grown FROM m;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "m");
	REQUIRE(Rows.size() == 1);
	REQUIRE(std::stod(Rows[0].at("r")) == AstralTest::Approx(3.0).epsilon(0.001));
	REQUIRE(std::stod(Rows[0].at("mu")) == AstralTest::Approx(2.0).epsilon(0.001));
	REQUIRE(Rows[0].at("grown").find("L[4]:") == 0);
}

TEST_CASE("AdvancedTypes: LIST wire format") {
	const auto L = AstralDB::AdvancedTypes::ParseListCell("L[2]:a,b");
	REQUIRE(L.has_value());
	REQUIRE(L->size() == 2);
	REQUIRE((*L)[0] == "a");
}

TEST_CASE("MathSci: loss, similarity, MATVEC, sort, RNG") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ml_ops_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ml.log").string(), false);
	const char *Q =
	    "CREATE TABLE t (id INT, a LIST(DOUBLE), b LIST(DOUBLE), m MATRIX(2,2) FLOAT, v VECTOR(2) FLOAT); "
	    "INSERT INTO t VALUES (1, 'L[2]:1,0', 'L[2]:0,1', 'T[2,2]:1,0,0,1', 'V[2]:1,2'); "
	    "SELECT SETSEED(42) AS s, MSE_LOSS(a,b) AS mse, COSINE_SIM(a,b) AS cs, "
	    "LIST_SORT('L[3]:3,1,2') AS sorted, MATVEC(m, v) AS mv FROM t;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	Db->WithExclusiveBytecodeLock([&]() {
		const auto &Row = Db->Tables_.at("t").RowStore[0];
		REQUIRE(Row.contains("m"));
		REQUIRE(Row.contains("v"));
		const auto Mv = AstralDB::MathSciComplex::MatVecCellFromReal(Row.at("m"), Row.at("v"));
		REQUIRE(Mv.has_value());
		REQUIRE(Mv->find("V[2]:") == 0);
		REQUIRE(std::stod(Row.at("mse")) == AstralTest::Approx(1.0).epsilon(0.001));
		REQUIRE(std::stod(Row.at("cs")) == AstralTest::Approx(0.0).epsilon(0.001));
		REQUIRE(Row.at("sorted") == "L[3]:1,2,3");
		REQUIRE(Row.contains("mv"));
		REQUIRE(Row.at("mv").find("V[2]:") == 0);
	});
}

TEST_CASE("SQL: INSERT VALUES with ST_MESH and ST_POLYGON expressions") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_geo_ins_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "geo_ins.log").string(), false);
	const char *Q =
	    "CREATE TABLE meshes (id INT, shape MESH); "
	    "INSERT INTO meshes (id, shape) VALUES (1, ST_MESH('V[9]:0,0,0,1,0,0,0,1,0', 'L[3]:0,1,2')); "
	    "CREATE TABLE parcels (id INT, geom POLYGON); "
	    "INSERT INTO parcels VALUES (1, ST_POLYGON('V[10]:0,0,2,0,2,2,0,2,0,0')); "
	    "SELECT ST_MESH_VOLUME(shape) AS vol FROM meshes WHERE id = 1; "
	    "SELECT ST_GEOM_AREA(geom) AS area FROM parcels WHERE id = 1;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::Basic);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	Db->WithExclusiveBytecodeLock([&]() {
		REQUIRE(Db->Tables_.at("meshes").RowStore.size() == 1);
		REQUIRE(Db->Tables_.at("parcels").RowStore.size() == 1);
		REQUIRE(!Db->Tables_.at("meshes").RowStore[0].at("shape").empty());
		REQUIRE(!Db->Tables_.at("parcels").RowStore[0].at("geom").empty());
	});
}

TEST_CASE("SQL: JSON NULLIF AS OF MATCH_RECOGNIZE") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_std_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "std.log").string(), false);
	const char *Q =
	    "CREATE TABLE e (id INT, valid_from TEXT, valid_to TEXT, k TEXT); "
	    "INSERT INTO e VALUES (1,'0','','A'),(2,'0','','B'); "
	    "SELECT JSON_EXTRACT('{\"n\":7}', 'n') AS j, NULLIF(1,1) AS n FROM e; "
	    "SELECT id FROM e MATCH_RECOGNIZE (ORDER BY id PATTERN (A) DEFINE A AS k = 'A');";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
}

TEST_CASE("SQL: text search GROUPING_ID XML") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_txt_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "txt.log").string(), false);
	const char *Q =
	    "CREATE TABLE docs (id INT, body TEXT); "
	    "INSERT INTO docs VALUES (1,'alpha beta'),(2,'gamma delta'); "
	    "SELECT id FROM docs WHERE body MATCH 'beta'; "
	    "SELECT TEXT_CONTAINS(body,'gamma') AS hit FROM docs WHERE id = 2; "
	    "INSERT INTO docs VALUES (3,'<r><t>x</t></r>'); "
	    "SELECT XML_VALID(body) AS v, XML_EXTRACT(body,'r.t') AS x FROM docs WHERE id = 3; "
	    "CREATE TABLE emb (id INT, v VECTOR(2)); INSERT INTO emb VALUES (1,'V[2]:1,0'),(2,'V[2]:0,1'); "
	    "CREATE INDEX ev ON emb (v) USING VECTOR; "
	    "SELECT VECTOR_TOPK('ev','V[2]:1,0',1) AS top FROM emb; "
	    "DROP INDEX ev;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
}

TEST_CASE("SQL: ARRAY/MDARRAY aliases and ISO JSON/XML spellings") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_isojx_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "isojx.log").string(), false);
	const char *Q =
	    "CREATE TABLE iso_types (id INT, a ARRAY(INT), m MDARRAY(2,2) FLOAT); "
	    "INSERT INTO iso_types VALUES (1, 'L[3]:1,2,3', 'T[2,2]:1,0,0,1'); "
	    "CREATE TABLE iso_docs (id INT, j TEXT, x TEXT); "
	    "INSERT INTO iso_docs VALUES (1, '{\"n\":7,\"obj\":{\"k\":\"v\"}}', '<r><t>x</t></r>'); "
	    "SELECT JSON_VALUE(j, 'n') AS jv, JSON_QUERY(j, 'obj') AS jq, JSON_EXISTS(j, 'obj') AS je FROM iso_docs; "
	    "SELECT XMLQUERY(x, 'r.t') AS xq, XMLSERIALIZE(x) AS xs, XMLEXISTS(x, 'r.t') AS xe FROM iso_docs;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
}

TEST_CASE("SQL: CREATE TYPE and CREATE TABLE OF") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_type_table_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_typed.log").string(), false);
	const char *Code =
	    "CREATE TYPE customer_t AS (id INT, name TEXT);"
	    "CREATE TABLE customers OF customer_t;"
	    "INSERT INTO customers VALUES (7, 'eve');";
	AstralDB::SQL::Parser P(Code);
	auto Bc = AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Bc));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(TableExists(Db, "customers"));
	auto Rows = AllRows(Db, "customers");
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0].at("id") == "7");
	REQUIRE(Rows[0].at("name") == "eve");
}

TEST_CASE("SQL: DROP TYPE rejects referenced typed tables") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_type_drop_guard_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "_typed_drop.log").string(), false);
	const char *Setup =
	    "CREATE TYPE customer_t AS (id INT, name TEXT);"
	    "CREATE TABLE customers OF customer_t;";
	AstralDB::SQL::Parser P0(Setup);
	auto B0 = AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(B0));

	const char *Drop = "DROP TYPE customer_t;";
	AstralDB::SQL::Parser P1(Drop);
	auto B1 = AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE_THROWS_AS(I.Execute(B1), std::runtime_error);
}

TEST_CASE("C API: open exec prepare step finalize") {
	fs::path Dir = UniqueTempDir("astral_capi_");
	const fs::path DbPath = Dir / "astral_capi.db";
	astraldb_t *Db = nullptr;
	REQUIRE(astraldb_open(DbPath.string().c_str(), &Db) == 0);
	REQUIRE(Db != nullptr);
	REQUIRE(astraldb_exec(Db, "CREATE TABLE capi_t (id INT, name TEXT);") == 0);
	REQUIRE(astraldb_exec(Db, "INSERT INTO capi_t VALUES (1, 'alpha');") == 0);
	astraldb_stmt_t *Stmt = nullptr;
	REQUIRE(astraldb_prepare(Db, "SELECT id, name FROM capi_t;", &Stmt) == 0);
	REQUIRE(Stmt != nullptr);
	REQUIRE(astraldb_stmt_step(Stmt) == 1);
	REQUIRE(astraldb_stmt_column_count(Stmt) == 2);
	REQUIRE(std::string(astraldb_stmt_column_name(Stmt, 0)) == "id");
	REQUIRE(std::string(astraldb_stmt_column_text(Stmt, 0)) == "1");
	REQUIRE(std::string(astraldb_stmt_column_text(Stmt, 1)) == "alpha");
	REQUIRE(astraldb_stmt_step(Stmt) == 0);
	astraldb_stmt_finalize(Stmt);
	astraldb_close(Db);
}

TEST_CASE("C API: prepare rejects non-SELECT SQL") {
	fs::path Dir = UniqueTempDir("astral_capi_prepare_guard_");
	const fs::path DbPath = Dir / "astral_capi_guard.db";
	astraldb_t *Db = nullptr;
	REQUIRE(astraldb_open(DbPath.string().c_str(), &Db) == 0);
	REQUIRE(Db != nullptr);
	REQUIRE(astraldb_exec(Db, "CREATE TABLE capi_guard (id INT);") == 0);
	astraldb_stmt_t *Stmt = reinterpret_cast<astraldb_stmt_t *>(0x1);
	REQUIRE(astraldb_prepare(Db, "INSERT INTO capi_guard VALUES (1);", &Stmt) == -1);
	REQUIRE(Stmt == nullptr);
	const char *Err = astraldb_last_error(Db);
	REQUIRE(Err != nullptr);
	REQUIRE(std::string(Err).find("SELECT statements only") != std::string::npos);
	astraldb_close(Db);
}

TEST_CASE("MathSci: signal FFT conv Laplacian autograd") {
	REQUIRE(AstralDB::MathSci::LookupBuiltin("CONV1D").has_value());
	const auto ConvDirect = AstralDB::MathSci::EvalScalar(AstralDB::SQL::ScalarSqlFn::Conv1d,
	                                                      {"L[3]:1,2,3", "L[2]:1,1"});
	REQUIRE(ConvDirect.has_value());
	REQUIRE(ConvDirect->find("L[4]:") == 0);
	const auto GradMul = AstralDB::MathSci::EvalScalar(AstralDB::SQL::ScalarSqlFn::AdGradMulLhs,
	                                                   {"L[2]:2,3", "L[2]:4,5", "L[2]:1,1"});
	REQUIRE(GradMul.has_value());
	REQUIRE(GradMul->find("L[2]:") == 0);

	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_signal_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "sig.log").string(), false);
	const char *Q =
	    "CREATE TABLE s (id INT, a LIST(DOUBLE), b LIST(DOUBLE)); "
	    "INSERT INTO s VALUES (1, 'L[3]:1,2,3', 'L[2]:1,1'); "
	    "SELECT CONV_FULL(a, b) AS conv, LAPLACIAN(a) AS lap FROM s;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	size_t ScalarEvalOps = 0;
	for(const auto &Ins : Code) {
		if(Ins.Opcode_ == AstralDB::SQL::Opcode::SCALAR_FUNC_EVAL)
			++ScalarEvalOps;
	}
	REQUIRE(ScalarEvalOps >= 2);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	Db->WithExclusiveBytecodeLock([&]() {
		const auto &Tbl = Db->Tables_.at("s").RowStore;
		REQUIRE(Tbl.size() == 1);
		REQUIRE(Tbl[0].at("conv").find("L[4]:") == 0);
		REQUIRE(Tbl[0].at("lap").find("L[3]:") == 0);
	});

	const std::vector<double> Impulse{1.0, 0.0, 0.0, 0.0};
	const auto Spectrum = AstralDB::MathSciSignal::FftInterleavedReIm(Impulse);
	REQUIRE(Spectrum.size() == 8);
	const auto RoundTrip = AstralDB::MathSciSignal::IfftRealFromInterleaved(
	    std::vector<double>(Spectrum.begin(), Spectrum.end()));
	REQUIRE(RoundTrip.size() == 4);
	REQUIRE(RoundTrip[0] == AstralTest::Approx(1.0).epsilon(0.01));

	const std::vector<double> Ramp{0.0, 1.0, 2.0};
	const auto Dct = AstralDB::MathSciSignal::Dct2FromReal(Ramp);
	const auto Back = AstralDB::MathSciSignal::Idct2FromReal(Dct);
	REQUIRE(Back.size() == 3);
	REQUIRE(Back[1] == AstralTest::Approx(1.0).epsilon(0.05));
}

TEST_CASE("GeoSpatial: point parse distance and bbox") {
	const auto P = AstralDB::GeoSpatial::ParsePointCell("G(-73.98,40.75)");
	REQUIRE(P.has_value());
	REQUIRE(P->Lon == AstralTest::Approx(-73.98).epsilon(0.001));
	const auto Q = AstralDB::GeoSpatial::ParseWktPoint("POINT(0 0)");
	REQUIRE(Q.has_value());
	REQUIRE(AstralDB::GeoSpatial::HaversineMeters(*P, *Q) > 5'000'000.0);
	REQUIRE(AstralDB::GeoSpatial::WithinBbox(*P, -74.5, 40.0, -73.0, 41.0));
}

TEST_CASE("TimeSeriesCompression: round-trip values and series") {
	const std::vector<double> V{1.0, 1.5, 3.0};
	const std::string Blob = AstralDB::TimeSeriesCompression::CompressValues(V);
	REQUIRE(!Blob.empty());
	const auto Out = AstralDB::TimeSeriesCompression::DecompressValues(Blob);
	REQUIRE(Out.has_value());
	REQUIRE(Out->size() == V.size());
	REQUIRE((*Out)[2] == AstralTest::Approx(3.0).epsilon(0.001));
	const std::vector<double> E{1.0, 2.0, 4.0};
	const std::string SB = AstralDB::TimeSeriesCompression::CompressSeries(E, V);
	REQUIRE(!SB.empty());
	const auto Pair = AstralDB::TimeSeriesCompression::DecompressSeries(SB);
	REQUIRE(Pair.has_value());
	REQUIRE(Pair->first.size() == 3);
	REQUIRE(Pair->first[2] == AstralTest::Approx(4.0).epsilon(0.001));
}

TEST_CASE("Dataset: register table snapshot and bulk load") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ds_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ds.log").string(), false);
	const char *Q =
	    "CREATE TABLE s (id INT, a INT, b TEXT, c TEXT, d TEXT); INSERT INTO s BULK 10 START 1 STEP 1; "
	    "CREATE TABLE t (id INT, a INT, b TEXT, c TEXT, d TEXT); "
	    "CREATE DATASET d AS BULK 10 START 1 STEP 1; LOAD DATASET d INTO t; "
	    "CREATE DATASET snap AS TABLE s; CREATE TABLE u (id INT, a INT, b TEXT, c TEXT, d TEXT); "
	    "LOAD DATASET snap INTO u;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "s").size() == 10);
	REQUIRE(AllRows(Db, "t").size() == 10);
	REQUIRE(AllRows(Db, "u").size() == 10);
}

TEST_CASE("MemoryGuard: disabled until spike then allows under cap") {
	REQUIRE(!AstralDB::MemoryGuard::GuardsActive());
	AstralDB::MemoryGuard::NoteAlloc(1024);
	REQUIRE(!AstralDB::MemoryGuard::GuardsActive());
	{
		AstralDB::MemoryGuard::SpikeScope Scope;
		REQUIRE(AstralDB::MemoryGuard::GuardsActive());
	}
}

TEST_CASE("MemoryGuard: bounds check and session cap") {
	AstralDB::MemoryGuard::ResetSession();
	REQUIRE(AstralDB::MemoryGuard::SecureBoundsCheck(0, 100));
	REQUIRE(!AstralDB::MemoryGuard::SecureBoundsCheck(100, 100));
	REQUIRE(!AstralDB::MemoryGuard::SecureBoundsCheck(50, 40, 2));
	{
		AstralDB::MemoryGuard::SpikeScope Scope;
		const std::size_t Chunk = AstralDB::MemoryGuard::HardSessionCapBytes / 4 + 1;
		REQUIRE(AstralDB::MemoryGuard::AllowAlloc(Chunk));
		REQUIRE(AstralDB::MemoryGuard::AllowAlloc(Chunk));
		REQUIRE(AstralDB::MemoryGuard::AllowAlloc(Chunk));
		REQUIRE(!AstralDB::MemoryGuard::AllowAlloc(Chunk));
	}
	AstralDB::MemoryGuard::ResetSession();
	REQUIRE(AstralDB::MemoryGuard::EstimatedSessionBytes() == 0);
}

TEST_CASE("AdvancedTypes: VARIANT parse validate normalize") {
	const auto D = AstralDB::AdvancedTypes::ParseTypeSpelling("VARIANT");
	REQUIRE(D.has_value());
	REQUIRE(D->Family == AstralDB::AdvancedTypes::TypeFamily::Variant);
	REQUIRE(AstralDB::AdvancedTypes::ValidateCell(*D, "V{i:42}"));
	REQUIRE(!AstralDB::AdvancedTypes::ValidateCell(*D, "not-variant"));
	const auto N = AstralDB::AdvancedTypes::NormalizeCell(*D, "V{s:hello}");
	REQUIRE(N == "V{s:hello}");
}

TEST_CASE("GeoSpatial: terrain point and DEM sample") {
	const auto T = AstralDB::GeoSpatial::ParseTerrainCell("Z(-73.98,40.75,12.5)");
	REQUIRE(T.has_value());
	REQUIRE(T->ElevM == AstralTest::Approx(12.5).epsilon(0.001));
	const std::vector<double> Dem{0, 10, 20, 30};
	const auto Elev = AstralDB::GeoSpatial::SampleDemBilinear(Dem, 2, 2, 0, 0, 1, 1, 0.25, 0.25);
	REQUIRE(Elev.has_value());
	REQUIRE(*Elev == AstralTest::Approx(7.5).epsilon(0.01));
}

TEST_CASE("GeoSpatial: mesh glTF import/export and CSG") {
	using AstralDB::SQL::ScalarSqlFn;
	const auto Defined = AstralDB::MathSci::EvalScalar(
	    ScalarSqlFn::StMeshDefine, {"V[9]:0,0,0,1,0,0,0,1,0", "L[3]:0,1,2"});
	REQUIRE(Defined.has_value());
	REQUIRE(Defined->find("M3{") == 0);
	const auto Exported = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshExportGltf, {*Defined});
	REQUIRE(Exported.has_value());
	REQUIRE(Exported->find("\"meshes\"") != std::string::npos);
	const auto Imported = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshImportGltf, {*Exported});
	REQUIRE(Imported.has_value());
	REQUIRE(Imported->find("M3{") == 0);
	const auto Sewn = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshSew, {*Defined, "0.0001"});
	REQUIRE(Sewn.has_value());
	REQUIRE(Sewn->find("M3{") == 0);
	const auto U = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshUnion, {*Defined, *Imported});
	const auto I = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshIntersection, {*Defined, *Imported});
	const auto D = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshDifference, {*Defined, *Imported});
	REQUIRE(U.has_value());
	REQUIRE(I.has_value());
	REQUIRE(D.has_value());
	const auto Area = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshSurfaceArea, {*Defined});
	REQUIRE(Area.has_value());
	REQUIRE(std::stod(*Area) > 0.0);
	const auto Vol = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshVolume, {*Defined});
	REQUIRE(Vol.has_value());
	const auto Moved = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshTranslate, {*Defined, "1", "0", "0"});
	REQUIRE(Moved.has_value());
}

TEST_CASE("GeoSpatial: polygon metrics booleans and GeoJSON") {
	using AstralDB::SQL::ScalarSqlFn;
	const auto Square = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StPolygon, {"V[10]:0,0,2,0,2,2,0,2,0,0"});
	REQUIRE(Square.has_value());
	REQUIRE(Square->find("P2{") == 0);
	const auto Area = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomArea, {*Square});
	REQUIRE(Area.has_value());
	REQUIRE(std::stod(*Area) == AstralTest::Approx(4.0).epsilon(0.01));
	const auto Contains = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomContains, {*Square, "G(1,1)"});
	REQUIRE(Contains == "1");
	const auto Hole = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StPolygonWkt,
	                                                 {"POLYGON((1 0,3 0,3 2,1 2,1 0))"});
	REQUIRE(Hole.has_value());
	const auto Intersects = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomIntersects, {*Square, *Hole});
	REQUIRE(Intersects == "1");
	const auto U = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomUnion, {*Square, *Hole});
	REQUIRE(U.has_value());
	const auto Geo = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeojsonExport, {*Square});
	REQUIRE(Geo.has_value());
	const auto Back = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeojsonImport, {*Geo});
	REQUIRE(Back.has_value());
	REQUIRE(Back->find("P2{") == 0);
	const auto Buf = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomBuffer, {*Square, "0.5"});
	REQUIRE(Buf.has_value());
}

TEST_CASE("GeoSpatial: hardened validate repair and predicates") {
	using AstralDB::SQL::ScalarSqlFn;
	const auto Dup = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StPolygon,
	                                                {"V[12]:0,0,1,0,1,0,1,1,0,1,0,0"});
	REQUIRE(Dup.has_value());
	REQUIRE(AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomValidate, {*Dup}) == "1");
	const auto Repaired = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomRepair, {*Dup});
	REQUIRE(Repaired.has_value());
	REQUIRE(AstralDB::MathSci::EvalScalar(ScalarSqlFn::StGeomValidate, {*Repaired}) == "1");

	const auto Mesh = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshDefine,
	                                                {"V[9]:0,0,0,1,0,0,0,1,0", "L[3]:0,1,2"});
	REQUIRE(Mesh.has_value());
	REQUIRE(AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshValidate, {*Mesh}) == "1");
	const auto MeshRep = AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshRepair, {*Mesh});
	REQUIRE(MeshRep.has_value());
	REQUIRE(AstralDB::MathSci::EvalScalar(ScalarSqlFn::StMeshValidate, {*MeshRep}) == "1");

	REQUIRE(AstralDB::DS::Geometry2D::ValidatePolygon(
	            AstralDB::DS::Geometry2D::PolygonFromRing({0, 0, 1, 0, 1, 1, 0, 1, 0, 0}))
	            .Ok);
	REQUIRE(!AstralDB::DS::Geometry2D::ValidatePolygon(
	             AstralDB::DS::Geometry2D::PolygonFromRing({0, 0, 2, 2, 2, 0, 0, 2, 0, 0}))
	             .Ok);

	const auto Unit = AstralDB::DS::Geometry2D::PolygonFromRing({0, 0, 2, 0, 2, 2, 0, 2, 0, 0});
	const auto Bar = AstralDB::DS::Geometry2D::PolygonFromRing({1, 1, 3, 1, 3, 3, 1, 3, 1, 1});
	const auto I = AstralDB::DS::Geometry2D::Intersection(Unit, Bar);
	REQUIRE(AstralDB::DS::Geometry2D::ValidatePolygon(I).Ok);
	REQUIRE(AstralDB::DS::Geometry2D::PolygonArea(I) == AstralTest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("GQL graph: analytics shortest path PageRank projection varlen") {
	AstralDB::SQL::SetParserDiagnostics(false);
	const std::string Script = ReadExampleFile(RepoRoot() / "examples" / "graph_analytics.sql");
	REQUIRE(!Script.empty());
	fs::path Dir = UniqueTempDir("astral_gql2_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "gql2.log").string(), false);
	AstralDB::SQL::Parser P(Script);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::Basic);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "paths_1_3").size() >= 3);
	const auto Route = AllRows(Db, "route");
	REQUIRE(!Route.empty());
	REQUIRE(Route.front().at("found") == "1");
	REQUIRE(AllRows(Db, "ranks").size() == 5);
}

TEST_CASE("GQL graph: social BFS reach and finance labeled match") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_graph_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "graph.log").string(), false);
	const char *Social =
	    "CREATE TABLE users (id INT, name TEXT); CREATE TABLE follows (src INT, dst INT); "
	    "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol'), (4, 'Dave'); "
	    "INSERT INTO follows VALUES (1, 2), (1, 3), (2, 4), (3, 4); "
	    "CREATE GRAPH social VERTEX TABLE users (id) EDGE TABLE follows (src, dst); "
	    "GRAPH MATCH (a)-[e]->(b) IN social INTO all_edges; "
	    "GRAPH TRAVERSE FROM 1 IN social DEPTH 3 BFS INTO reach_alice;";
	AstralDB::SQL::Parser P(Social);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::Basic);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "all_edges").size() == 4);
	const auto Reach = AllRows(Db, "reach_alice");
	REQUIRE(Reach.size() == 4);
	std::set<std::string> Seen;
	for(const auto &Row : Reach) {
		auto It = Row.find("vertex_id");
		REQUIRE(It != Row.end());
		Seen.insert(It->second);
	}
	REQUIRE(Seen.count("1"));
	REQUIRE(Seen.count("2"));
	REQUIRE(Seen.count("3"));
	REQUIRE(Seen.count("4"));

	fs::path DirFin = UniqueTempDir("astral_graph_fin_");
	ScopedCwd CwdFin(DirFin);
	RemoveEphemeralDb(DirFin);
	AstralDB::Logger LogFin((DirFin / "graph_fin.log").string(), false);
	const char *Finance =
	    "CREATE TABLE accounts (id INT, name TEXT); "
	    "CREATE TABLE transfers (src INT, dst INT, kind TEXT, amount INT); "
	    "INSERT INTO accounts VALUES (100, 'Treasury'), (101, 'Alice'), (102, 'Bob'), (103, 'Carol'); "
	    "INSERT INTO transfers VALUES (100, 101, 'fund', 5000), (101, 102, 'payment', 120), "
	    "(102, 103, 'payment', 80), (103, 101, 'payment', 40); "
	    "CREATE GRAPH ledger VERTEX TABLE accounts (id) EDGE TABLE transfers (src, dst, kind); "
	    "GRAPH MATCH (a)-[e]->(b) IN ledger WHERE e.kind = 'payment' INTO payments; "
	    "GRAPH TRAVERSE FROM 101 IN ledger DEPTH 4 DFS INTO from_alice;";
	AstralDB::SQL::Parser P2(Finance);
	AstralDB::SQL::Bytecode Code2 =
	    AstralDB::SQL::BuildBytecode(&LogFin, AstralDB::SQL::OptimizationLevel::Basic);
	AstralDB::SQL::BytecodeInterpreter I2(&LogFin);
	REQUIRE_NOTHROW(I2.Execute(Code2));
	REQUIRE(I2.PrimaryDatabase() != nullptr);
	REQUIRE(AllRows(I2.PrimaryDatabase(), "payments").size() == 3);
	REQUIRE(AllRows(I2.PrimaryDatabase(), "from_alice").size() >= 3);
}

TEST_CASE("GQL graph: Cypher MATCH syntax") {
	AstralDB::SQL::SetParserDiagnostics(false);
	const std::string Script = ReadExampleFile(RepoRoot() / "examples" / "graph_cypher.sql");
	REQUIRE(!Script.empty());
	fs::path Dir = UniqueTempDir("astral_graph_cypher_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "cypher.log").string(), false);
	AstralDB::SQL::Parser P(Script);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::Basic);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	REQUIRE(I.PrimaryDatabase() != nullptr);
	REQUIRE(I.PrimaryDatabase()->GraphByName("social") != nullptr);
	REQUIRE(AllRows(I.PrimaryDatabase(), "cypher_edges").size() == 2);
	REQUIRE(AllRows(I.PrimaryDatabase(), "cypher_from_alice").size() == 1);
}

TEST_CASE("GQL graph: catalog persists across snapshot reopen") {
	fs::path Dir = UniqueTempDir("astral_graph_persist_");
	fs::path DbPath = Dir / "graph_persist.db";
	std::error_code Ec;
	fs::remove(DbPath, Ec);
	fs::remove(DbPath.string() + ".wal", Ec);
	{
		AstralDB::Database Db(DbPath, nullptr);
		AstralDB::Database::Schema U;
		AstralDB::Database::Column Cu;
		Cu.Name = "id";
		Cu.DefaultValue = "INT";
		Cu.IsPrimaryKey = true;
		U.push_back(Cu);
		Db.CreateTable("u", U).get();
		AstralDB::Database::Schema F;
		AstralDB::Database::Column Cs;
		Cs.Name = "src";
		Cs.DefaultValue = "INT";
		AstralDB::Database::Column Cd;
		Cd.Name = "dst";
		Cd.DefaultValue = "INT";
		F.push_back(Cs);
		F.push_back(Cd);
		Db.CreateTable("f", F).get();
		Db.Insert("u", {{"id", "1"}}).get();
		Db.Insert("u", {{"id", "2"}}).get();
		Db.Insert("f", {{"src", "1"}, {"dst", "2"}}).get();
		AstralDB::GraphSpec G;
		G.Name = "g";
		G.VertexTable = "u";
		G.VertexIdCol = "id";
		G.EdgeTable = "f";
		G.EdgeSrcCol = "src";
		G.EdgeDstCol = "dst";
		Db.RegisterGraph(std::move(G));
		REQUIRE(Db.GraphByName("g") != nullptr);
		Db.SyncToFile();
	}
	{
		AstralDB::Database Db2(DbPath, nullptr);
		REQUIRE(Db2.GraphByName("g") != nullptr);
		REQUIRE(Db2.GraphByName("g")->EdgeTable == "f");
		AstralDB::GraphMatchRequest Req;
		Req.GraphName = "g";
		Req.MinHops = 1;
		Req.MaxHops = 1;
		Req.ResultTable = "again";
		Db2.GraphMatch(Req);
		REQUIRE(AllRows(&Db2, "again").size() == 1);
	}
}

TEST_CASE("Dataset: versioning snapshots two generations") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_dsv_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "dsv.log").string(), false);
	const char *Q =
	    "CREATE TABLE s (id INT, a INT, b TEXT, c TEXT, d TEXT); INSERT INTO s BULK 5 START 1 STEP 1; "
	    "CREATE DATASET snap AS TABLE s; DELETE FROM s; INSERT INTO s BULK 10 START 1 STEP 1; CREATE DATASET snap AS TABLE s; "
	    "CREATE TABLE v1 (id INT, a INT, b TEXT, c TEXT, d TEXT); "
	    "CREATE TABLE v2 (id INT, a INT, b TEXT, c TEXT, d TEXT); "
	    "LOAD DATASET snap VERSION 1 INTO v1; LOAD DATASET snap VERSION 2 INTO v2;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "v1").size() == 5);
	REQUIRE(AllRows(Db, "v2").size() == 10);
}

TEST_CASE("Maintenance: VACUUM and REPACK CONCURRENTLY preserve rows") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_vac_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "vac.log").string(), false);
	const char *Q =
	    "CREATE TABLE t (id INT, x TEXT); INSERT INTO t (id, x) VALUES (1, 'a'), (2, 'b'); "
	    "VACUUM TABLE t; REPACK TABLE t CONCURRENTLY; SELECT id FROM t ORDER BY id;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	REQUIRE(AllRows(Db, "t").size() == 2);
}

TEST_CASE("Security: VARIANT injection rejected at storage") {
	fs::path Dir = UniqueTempDir("astral_varsec_");
	fs::path DbPath = Dir / "varsec.db";
	AstralDB::Database Db(DbPath, nullptr);
	AstralDB::Database::Schema Sch;
	AstralDB::Database::Column IdCol;
	IdCol.Name = "id";
	IdCol.DefaultValue = "INTEGER";
	AstralDB::Database::Column PayloadCol;
	PayloadCol.Name = "payload";
	PayloadCol.DefaultValue = "VARIANT";
	Sch.push_back(IdCol);
	Sch.push_back(PayloadCol);
	Db.CreateTable("v", Sch).get();
	REQUIRE_NOTHROW(Db.Insert("v", {{"id", "1"}, {"payload", "V{i:1}"}}).get());
	REQUIRE_THROWS_AS(Db.Insert("v", {{"id", "2"}, {"payload", "not-a-variant"}}).get(), std::runtime_error);
}

TEST_CASE("MathSci: extended solvers ST and TS SQL") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_ext_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "ext.log").string(), false);
	const char *Q =
	    "CREATE TABLE t (id INT, p POINT); INSERT INTO t (id, p) VALUES (1, 'G(0,0)'); "
	    "SELECT ST_DISTANCE_SPHERICAL(p, 'G(0,0.01)') AS d, "
	    "ODE_HEUN('L[2]:1,0','L[2]:0.1,0','L[2]:0.11,0','0.1') AS h, "
	    "TS_COMPRESS('L[3]:1,2,3') AS c FROM t;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
}

TEST_CASE("MathSci: Hessian Wirtinger and ODE SDE PDE solvers") {
	const auto Hess = AstralDB::MathSci::EvalScalar(AstralDB::SQL::ScalarSqlFn::AdHessianSquare,
	                                                {"L[2]:1,2", "L[2]:0.5,0.5"});
	REQUIRE(Hess.has_value());

	const auto Wz = AstralDB::MathSciAutograd::WirtingerDzFromCell("C(1,2)");
	REQUIRE(Wz.has_value());
	REQUIRE(Wz->size() == 2);

	const auto Euler = AstralDB::MathSciSolves::OdeEulerFromReal({1.0, 2.0}, 0.1, {0.5, -0.2});
	REQUIRE(Euler.size() == 2);
	REQUIRE(Euler[0] == AstralTest::Approx(1.05).epsilon(0.001));

	const auto Gbm = AstralDB::MathSciSolves::SdeGbmScalar(100.0, 0.0, 0.2, 0.01, 0.0);
	REQUIRE(Gbm == AstralTest::Approx(100.0 * std::exp(-0.0002)).epsilon(0.01));

	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_solves_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "sol.log").string(), false);
	const char *Q =
	    "CREATE TABLE q (id INT); INSERT INTO q VALUES (1); "
	    "SELECT ODE_EULER('L[2]:1,0', 'L[2]:0.1,0', '0.1') AS y1, "
	    "SDE_GBM(10, 0, 0.1, 0.01, 0) AS gbm FROM q;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	Db->WithExclusiveBytecodeLock([&]() {
		const auto &Row = Db->Tables_.at("q").RowStore[0];
		REQUIRE(Row.at("y1").find("L[2]:") == 0);
		REQUIRE(std::stod(Row.at("gbm")) == AstralTest::Approx(10.0 * std::exp(-0.00005)).epsilon(0.01));
	});
}

TEST_CASE("MathSci: iterative linear root and march solvers") {
	const auto Rk3 = AstralDB::MathSciSolves::OdeRk3FromReal({1.0}, 0.1, {0.1}, {0.12}, {0.13});
	REQUIRE(Rk3.size() == 1);
	REQUIRE(Rk3[0] == AstralTest::Approx(1.012).epsilon(0.01));

	const auto Ab2 = AstralDB::MathSciSolves::OdeAdamsBashforth2FromReal({1.0}, 0.1, {0.2}, {0.1});
	REQUIRE(Ab2[0] == AstralTest::Approx(1.015).epsilon(0.01));

	const std::string Mat = AstralDB::AdvancedTypes::FormatMatrixCell({4.0, 1.0, 1.0, 3.0}, 2, 2);
	const auto Cg = AstralDB::MathSciSolves::LinearCgSolveFromMat(Mat, {0.0, 0.0}, {1.0, 2.0}, 32, 1e-6);
	REQUIRE(Cg.size() == 2);
	REQUIRE(Cg[0] == AstralTest::Approx(0.0909).epsilon(0.02));
	REQUIRE(Cg[1] == AstralTest::Approx(0.6364).epsilon(0.02));

	const auto Jac = AstralDB::MathSciSolves::LinearJacobiStepFromMat(Mat, {0.0, 0.0}, {1.0, 2.0});
	REQUIRE(Jac.size() == 2);

	const auto Root = AstralDB::MathSciSolves::RootNewtonStepFromReal(2.0, 1.0, 2.0);
	REQUIRE(Root == AstralTest::Approx(1.5).epsilon(1e-6));

	const auto Marched = AstralDB::MathSciSolves::OdeMarchFromReal("EULER", {1.0}, 0.1, {0.5}, {0.0}, {0.0}, {0.0}, 2);
	REQUIRE(Marched[0] == AstralTest::Approx(1.1).epsilon(0.001));
}

TEST_CASE("MathSci: classifiers NLP embeddings integrators") {
	const auto Trap = AstralDB::MathSciSolves::OdeTrapezoidFromReal({1.0}, 0.2, {0.5}, {0.6});
	REQUIRE(Trap.size() == 1);
	REQUIRE(Trap[0] == AstralTest::Approx(1.11).epsilon(0.001));

	const auto Semi = AstralDB::MathSciSolves::OdeSemiImplicitFromReal({1.0}, 0.1, {0.5}, {0.2});
	REQUIRE(Semi[0] == AstralTest::Approx(1.07).epsilon(0.001));

	const auto Cn = AstralDB::MathSciSolves::OdeCrankNicolsonFromReal({1.0}, 0.1, {0.5}, {0.2});
	REQUIRE(Cn[0] == AstralTest::Approx(1.0359).epsilon(0.01));

	const auto Label = AstralDB::MathSciClassify::LinearLabelFromReal({1.0, 0.0}, {0.5, 1.0}, 0.0);
	REQUIRE(Label == "1");

	const auto Tok = AstralDB::MathSciNlp::TokenizeCellFromReal("Hello, world!");
	REQUIRE(Tok.has_value());
	REQUIRE(Tok->find("L[2]:") == 0);

	const auto Table = AstralDB::MathSciEmbeddings::BuildFromTokensAndMatrix(
	    {"a", "b"}, {1.0, 0.0, 0.0, 1.0}, 2, 2, false);
	REQUIRE(Table.has_value());
	const std::string Wire = AstralDB::MathSciEmbeddings::Serialize(*Table);
	const auto Round = AstralDB::MathSciEmbeddings::Deserialize(Wire);
	REQUIRE(Round.has_value());
	REQUIRE(Round->Dim == 2);
	REQUIRE(Round->Rows.size() == 2);

	AstralDB::MathSciEmbeddings::EmbeddingCache Lru(8);
	Lru.Insert("a", {1.f, 0.f});
	REQUIRE(Lru.Lookup("a").has_value());

	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_nlp_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "nlp.log").string(), false);
	const char *Q =
	    "CREATE TABLE t (id INT, doc TEXT); INSERT INTO t VALUES (1,'hello world'); "
	    "SELECT NLP_TOKENIZE(doc) AS toks, "
	    "ODE_TRAPEZOID('L[1]:1', 'L[1]:0.5', 'L[1]:0.6', '0.2') AS trap, "
	    "CLASSIFY_LINEAR('L[2]:1,0', 'L[2]:0.5,1', '0') AS lbl FROM t;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
}

TEST_CASE("MathSci: complex vectors unit") {
	const auto CvA = AstralDB::MathSciComplex::ParseNumericVec("CV[2]:1,0,0,1");
	const auto CvB = AstralDB::MathSciComplex::ParseNumericVec("CV[2]:0,1,1,0");
	REQUIRE(CvA.has_value());
	REQUIRE(CvB.has_value());
	REQUIRE(CvA->IsComplex());
	const auto Dot = AstralDB::MathSciComplex::DotComplex(*CvA, *CvB);
	REQUIRE(Dot.has_value());
	REQUIRE(Dot->first == AstralTest::Approx(0.0).epsilon(1e-5));
	REQUIRE(Dot->second == AstralTest::Approx(0.0).epsilon(1e-5));
	const auto Sim = AstralDB::MathSciComplex::CosineSim(*CvA, *CvB);
	REQUIRE(Sim.has_value());
	REQUIRE(*Sim == AstralTest::Approx(0.0).epsilon(1e-5));

	const auto CTable = AstralDB::MathSciEmbeddings::BuildFromTokensAndMatrix(
	    {"x", "y"}, {1.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f}, 2, 2, true);
	REQUIRE(CTable.has_value());
	const std::string CWire = AstralDB::MathSciEmbeddings::Serialize(*CTable);
	REQUIRE(CWire.rfind("EC[2,2]:", 0) == 0);
	REQUIRE(AstralDB::MathSciEmbeddings::Deserialize(CWire).has_value());
}

TEST_CASE("MathSci: CREATE EMBEDDING catalog") {
	fs::path Dir = UniqueTempDir("astral_cemb_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "cemb.log").string(), false);
	const char *Q =
	    "CREATE TABLE vocab (tok TEXT, vec TEXT); "
	    "INSERT INTO vocab VALUES ('a', 'CV[2]:1,0,0,1'), ('b', 'CV[2]:0,1,1,0'); "
	    "CREATE EMBEDDING emb AS TABLE vocab (tok, vec);";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
	AstralDB::SQL::BytecodeInterpreter I(&Log);
	REQUIRE_NOTHROW(I.Execute(Code));
	AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	Db->WithExclusiveBytecodeLock([&]() {
		REQUIRE(Db->EmbeddingWireCellAssumeDbMutexHeld("emb").has_value());
		const auto Wire = Db->EmbeddingWireCellAssumeDbMutexHeld("emb");
		REQUIRE(Wire->rfind("EC[2,2]:", 0) == 0);
		const auto Vec = AstralDB::MathSciEmbeddings::LookupCellFromReal(*Wire, "a", Db);
		REQUIRE(Vec.has_value());
		REQUIRE(Vec->find("CV[2]:") == 0);
	});
}

TEST_CASE("Simd: dot product matches scalar reference") {
	std::vector<float> A{1.f, 2.f, 3.f, 4.f};
	std::vector<float> B{2.f, 3.f, 4.f, 5.f};
	float Ref = 0.f;
	for(size_t I = 0; I < A.size(); ++I)
		Ref += A[I] * B[I];
	const float Sim = AstralDB::Simd::DotProductF32(A.data(), B.data(), A.size());
	REQUIRE(Sim == AstralTest::Approx(Ref).epsilon(0.001));
}

TEST_CASE("Superfetch: arms on large scans and stages prefetch") {
	AstralDB::Superfetch::ResetTelemetry();
	using Item = AstralDB::Database::Item;
	using Table = AstralDB::Database::Table;
	Table Rows;
	Rows.reserve(128);
	for(int I = 0; I < 128; ++I) {
		Item R;
		R["id"] = std::to_string(I);
		R["payload"] = std::string(48, 'a');
		Rows.push_back(std::move(R));
	}
	AstralDB::Superfetch::RowScanSession Scan;
	AstralDB::Superfetch::BeginRowScan(Rows, Scan);
	REQUIRE(Scan.Armed);
	std::size_t Bytes = 0;
	for(std::size_t Ri = 0; Ri < Rows.size(); ++Ri) {
		AstralDB::Superfetch::AdvanceRowScan(Rows, Ri, Scan);
		Bytes += Rows[Ri].at("payload").size();
	}
	REQUIRE(Bytes == 128u * 48u);
	const auto Stats = AstralDB::Superfetch::Stats();
	REQUIRE(Stats.PrefetchIssued > 0);
	Table Tiny{{{"x", "1"}}};
	AstralDB::Superfetch::RowScanSession TinyScan;
	AstralDB::Superfetch::BeginRowScan(Tiny, TinyScan);
	REQUIRE_FALSE(TinyScan.Armed);
}

namespace {

AstralDB::SQL::Bytecode BuildSqlBytecode(const std::string &Sql, AstralDB::SQL::OptimizationLevel Level) {
	AstralDB::Logger Log("opt_unit.log", false);
	AstralDB::SQL::Parser P(Sql);
	return AstralDB::SQL::BuildBytecode(&Log, Level);
}

bool RecursiveCteControlFlowOk(const AstralDB::SQL::Bytecode &Code) {
	if(!AstralDB::SQL::ValidateBytecodeControlFlow(Code))
		return false;
	for(size_t I = 0; I < Code.size(); ++I) {
		if(Code[I].Opcode_ != AstralDB::SQL::Opcode::RECURSIVE_CTE_FIXPOINT)
			continue;
		if(Code[I].Operands.size() < 4)
			return false;
		const auto *LoopStart = std::get_if<int64_t>(&Code[I].Operands[3]);
		if(!LoopStart || *LoopStart < 0)
			return false;
		if(static_cast<size_t>(*LoopStart) >= I)
			return false;
	}
	return true;
}

bool BytecodeHasPushInt64(const AstralDB::SQL::Bytecode &Code, int64_t Value) {
	for(const AstralDB::SQL::Instruction &Inst : Code) {
		if(Inst.Opcode_ != AstralDB::SQL::Opcode::PUSH || Inst.Operands.size() != 1)
			continue;
		if(const auto *V = std::get_if<int64_t>(&Inst.Operands[0])) {
			if(*V == Value)
				return true;
		}
	}
	return false;
}

void ExecuteSqlInDir(const fs::path &Dir, const std::string &Sql, AstralDB::SQL::OptimizationLevel Level) {
	AstralDB::Logger Log((Dir / "run.log").string(), false);
	AstralDB::SQL::Parser P(Sql);
	AstralDB::SQL::Bytecode Code = AstralDB::SQL::BuildBytecode(&Log, Level);
	REQUIRE(!Code.empty());
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astral.db");
	REQUIRE_NOTHROW(Interp.Execute(Code));
}

} // namespace

TEST_CASE("optimizer: O0 skips pipeline and O1 folds literal triples") {
	AstralDB::SQL::Bytecode Raw = {
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::PUSH, static_cast<int64_t>(2)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::PUSH, static_cast<int64_t>(3)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::ADD),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::HALT),
	};
	const AstralDB::SQL::Bytecode Unopt = Raw;
	AstralDB::SQL::RunOptimizerPipeline(Raw, AstralDB::SQL::OptimizationLevel::None);
	REQUIRE(Raw == Unopt);
	AstralDB::SQL::RunOptimizerPipeline(Raw, AstralDB::SQL::OptimizationLevel::Basic);
	REQUIRE(Raw.size() == 2);
	REQUIRE(Raw[0].Opcode_ == AstralDB::SQL::Opcode::PUSH);
	REQUIRE(BytecodeHasPushInt64(Raw, 5));
	REQUIRE(Raw[1].Opcode_ == AstralDB::SQL::Opcode::HALT);
}

TEST_CASE("optimizer: peephole folds double-NOT and identical-literal compares") {
	AstralDB::SQL::Bytecode Code = {
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::PUSH, static_cast<int64_t>(7)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::NOT),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::NOT),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::PUSH, static_cast<int64_t>(4)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::PUSH, static_cast<int64_t>(4)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::EQ),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::HALT),
	};
	AstralDB::SQL::RunOptimizerPipeline(Code, AstralDB::SQL::OptimizationLevel::Basic);
	REQUIRE(Code.size() == 3);
	REQUIRE(Code[0].Opcode_ == AstralDB::SQL::Opcode::PUSH);
	REQUIRE(BytecodeHasPushInt64(Code, 7));
	REQUIRE(BytecodeHasPushInt64(Code, 1));
}

TEST_CASE("optimizer: all levels preserve recursive CTE control flow") {
	const std::string Script = ReadExampleFile(RepoRoot() / "examples" / "sql99_recursive.sql");
	REQUIRE(!Script.empty());
	const AstralDB::SQL::OptimizationLevel Levels[] = {
	    AstralDB::SQL::OptimizationLevel::Basic,
	    AstralDB::SQL::OptimizationLevel::Advanced,
	    AstralDB::SQL::OptimizationLevel::Aggressive,
	    AstralDB::SQL::OptimizationLevel::Maximum,
	};
	bool SawFixpoint = false;
	for(AstralDB::SQL::OptimizationLevel Level : Levels) {
		INFO("level=", static_cast<int>(Level));
		AstralDB::SQL::Bytecode Code = BuildSqlBytecode(Script, Level);
		REQUIRE(!Code.empty());
		REQUIRE(RecursiveCteControlFlowOk(Code));
		for(size_t I = 0; I < Code.size(); ++I) {
			if(Code[I].Opcode_ == AstralDB::SQL::Opcode::RECURSIVE_CTE_FIXPOINT)
				SawFixpoint = true;
		}
	}
	REQUIRE(SawFixpoint);
}

TEST_CASE("optimizer: Maximum shrinks bytecode versus O0 on simple script") {
	const std::string Sql =
	    "CREATE TABLE lit (x INT); INSERT INTO lit VALUES (1); INSERT INTO lit VALUES (2);";
	const AstralDB::SQL::Bytecode Raw = BuildSqlBytecode(Sql, AstralDB::SQL::OptimizationLevel::None);
	const AstralDB::SQL::Bytecode Max = BuildSqlBytecode(Sql, AstralDB::SQL::OptimizationLevel::Maximum);
	REQUIRE(AstralDB::SQL::ValidateBytecodeControlFlow(Raw));
	REQUIRE(AstralDB::SQL::ValidateBytecodeControlFlow(Max));
	REQUIRE(Max.size() <= Raw.size());
}

TEST_CASE("optimizer: Maximum matches O0 row counts on DML") {
	const std::string Sql = "CREATE TABLE om (v INT); INSERT INTO om VALUES (1), (2), (3);";
	fs::path DirNone = UniqueTempDir("astral_opt_o0_");
	fs::path DirMax = UniqueTempDir("astral_opt_o4_");
	ExecuteSqlInDir(DirNone, Sql, AstralDB::SQL::OptimizationLevel::None);
	ExecuteSqlInDir(DirMax, Sql, AstralDB::SQL::OptimizationLevel::Maximum);
	AstralDB::Logger LogNone((DirNone / "n.log").string(), false);
	AstralDB::Logger LogMax((DirMax / "m.log").string(), false);
	AstralDB::SQL::BytecodeInterpreter INone(&LogNone);
	AstralDB::SQL::BytecodeInterpreter IMax(&LogMax);
	INone.DatabasePath(DirNone / "astral.db");
	IMax.DatabasePath(DirMax / "astral.db");
	INone.EnsurePrimaryDatabaseOpened();
	IMax.EnsurePrimaryDatabaseOpened();
	REQUIRE(INone.PrimaryDatabase() != nullptr);
	REQUIRE(IMax.PrimaryDatabase() != nullptr);
	REQUIRE(AllRows(INone.PrimaryDatabase(), "om").size() == 3);
	REQUIRE(AllRows(IMax.PrimaryDatabase(), "om").size() == 3);
}

TEST_CASE("optimizer: SAVEPOINT survives Maximum optimization") {
	AstralDB::SQL::SetParserDiagnostics(false);
	fs::path Dir = UniqueTempDir("astral_opt_sp_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "sp.log").string(), false);
	const char *Q = "CREATE TABLE sp (x INT); INSERT INTO sp VALUES (10); SAVEPOINT a; "
	                  "UPDATE sp SET x = 99 WHERE x = 10; ROLLBACK TO SAVEPOINT a;";
	AstralDB::SQL::Parser P(Q);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::Maximum);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astral.db");
	REQUIRE_NOTHROW(Interp.Execute(Code));
	REQUIRE(Interp.PrimaryDatabase() != nullptr);
	REQUIRE(AllRows(Interp.PrimaryDatabase(), "sp").size() == 1);
	REQUIRE(AllRows(Interp.PrimaryDatabase(), "sp")[0].at("x") == "10");
}

TEST_CASE("optimizer: invalid control flow after pass reverts bytecode") {
	AstralDB::SQL::Bytecode Code = {
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::PUSH, static_cast<int64_t>(0)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::NOP),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::RECURSIVE_CTE_FIXPOINT, std::string("w"),
	                                  std::string("d"), static_cast<int64_t>(4), static_cast<int64_t>(99)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::HALT),
	};
	const size_t Before = Code.size();
	AstralDB::SQL::RunOptimizerPipeline(Code, AstralDB::SQL::OptimizationLevel::Basic);
	REQUIRE(!AstralDB::SQL::ValidateBytecodeControlFlow(Code));
	REQUIRE(Code.size() == Before);
}

TEST_CASE("optimizer: jump threading collapses jmp chains" * doctest::test_suite("fast")) {
	AstralDB::SQL::Bytecode Code = {
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::JMP, static_cast<int64_t>(1)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::JMP, static_cast<int64_t>(2)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::PUSH, static_cast<int64_t>(1)),
	    AstralDB::SQL::MakeInstruction(AstralDB::SQL::Opcode::HALT),
	};
	AstralDB::SQL::RunOptimizerPipeline(Code, AstralDB::SQL::OptimizationLevel::Advanced);
	REQUIRE(Code[0].Opcode_ == AstralDB::SQL::Opcode::JMP);
	const auto *T = std::get_if<int64_t>(&Code[0].Operands[0]);
	REQUIRE(T != nullptr);
	REQUIRE(*T == 2);
}

TEST_CASE("perf: nuke.sql bulk and window on ephemeral db" * doctest::test_suite("perf")) {
	AstralDB::SQL::SetParserDiagnostics(false);
	std::string Script = ReadExampleFile(RepoRoot() / "examples" / "nuke.sql");
	{
		const std::string Needle = "BULK 10000000";
		const std::string Repl = "BULK 200000";
		if(const std::size_t Pos = Script.find(Needle); Pos != std::string::npos)
			Script.replace(Pos, Needle.size(), Repl);
	}
	REQUIRE(!Script.empty());
	fs::path Dir = UniqueTempDir("astral_nuke_perf_");
	ScopedCwd Cwd(Dir);
	RemoveEphemeralDb(Dir);
	AstralDB::Logger Log((Dir / "nuke.log").string(), false);
	AstralDB::SQL::Parser P(Script);
	AstralDB::SQL::Bytecode Code =
	    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::Maximum);
	AstralDB::SQL::BytecodeInterpreter Interp(&Log);
	Interp.DatabasePath(Dir / "astraldb_session_nuke.db");
	const auto T0 = std::chrono::steady_clock::now();
	REQUIRE_NOTHROW(Interp.Execute(Code));
	const double Ms =
	    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	REQUIRE(Ms < 120000.0);
}

TEST_CASE("examples: every *.sql parses, compiles, runs") {
	if(!RunExamplesSweep())
		return;
	AstralDB::SQL::SetParserDiagnostics(false);
	AstralTest::PerfSection Perf("examples/*.sql (all files)");
	const fs::path Ex = RepoRoot() / "examples";
	REQUIRE(fs::is_directory(Ex));
	std::vector<fs::path> Files;
	for(const auto &Ent : fs::directory_iterator(Ex)) {
		const auto &Pe = Ent.path();
		if(Ent.is_regular_file() && Pe.extension() == ".sql")
			Files.push_back(Pe);
	}
	REQUIRE(!Files.empty());
	std::sort(Files.begin(), Files.end());
	fs::path Dir = UniqueTempDir("astral_ex_all_");
	ScopedCwd Cwd(Dir);
	AstralDB::Logger Log((Dir / "ex.log").string(), false);
	for(const fs::path &F : Files) {
		const std::string Name = F.filename().string();
		if(IsPerfHarness(Name))
			continue;
		if(!RunAllExamplesInDefaultSuite() && IsDefaultSlowExample(Name))
			continue;
		INFO("example: ", Name);
		RemoveEphemeralDb(Dir);
		const std::string Script = ReadExampleFile(F);
		REQUIRE_MESSAGE(!Script.empty(), Name.c_str());
		AstralDB::SQL::Parser P(Script);
		AstralDB::SQL::Bytecode Code =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::Basic);
		REQUIRE_MESSAGE(!Code.empty(), Name.c_str());
		AstralDB::SQL::BytecodeInterpreter Interp(&Log);
		REQUIRE_NOTHROW(Interp.Execute(Code));
	}
}
