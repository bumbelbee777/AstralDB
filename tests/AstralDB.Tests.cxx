/* Database before doctest so schema types are parsed before short macro names leak in. */
#include <Database/AdvancedTypes.hxx>
#include <Database/Database.hxx>
#include <Database/ColumnarStorage.hxx>
#include <Database/HybridStorageScheduler.hxx>
#include <Database/HybridTable.hxx>
#include <Database/MathSci.hxx>
#include <Database/MathSciAutograd.hxx>
#include <Database/MathSciSignal.hxx>
#include <Database/MathSciSolves.hxx>
#include <Database/WriteAheadLog.hxx>
#include <DS/ErrorCorrection.hxx>
#include <IO/Logger.hxx>
#include <IO/SIMD.hxx>
#include <SQL/SQL.hxx>
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include <SQL/BytecodeFormat.hxx>
#include <SQL/BytecodeInspect.hxx>
#include <SQL/BytecodeDebug.hxx>
#include <SQL/BytecodeProcedures.hxx>
#include "AstralTestHelpers.hxx"
#include <algorithm>
#include <chrono>
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

} // namespace

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
		if(Inst.Opcode == AstralDB::SQL::Opcode::INSERT_BULK)
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
		if(In.Opcode == AstralDB::SQL::Opcode::WINDOW_ROW_NUMBER)
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
		        return In.Opcode == AstralDB::SQL::Opcode::SLICE_RANGE;
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
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "adv");
	REQUIRE(Rows.size() == 1);
	REQUIRE(std::stod(Rows[0].at("dot")) == AstralTest::Approx(3.0).epsilon(0.001));
	REQUIRE(Rows[0].at("cm").find("C(") == 0);
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
	const AstralDB::Database *Db = I.PrimaryDatabase();
	REQUIRE(Db != nullptr);
	const auto Rows = AllRows(Db, "t");
	REQUIRE(Rows.size() == 1);
	REQUIRE(std::stod(Rows[0].at("mse")) == AstralTest::Approx(1.0).epsilon(0.001));
	REQUIRE(std::stod(Rows[0].at("cs")) == AstralTest::Approx(0.0).epsilon(0.001));
	REQUIRE(Rows[0].at("sorted") == "L[3]:1,2,3");
	REQUIRE(Rows[0].at("mv").find("V[2]:") == 0);
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
		if(Ins.Opcode == AstralDB::SQL::Opcode::SCALAR_FUNC_EVAL)
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

TEST_CASE("Simd: dot product matches scalar reference") {
	std::vector<float> A{1.f, 2.f, 3.f, 4.f};
	std::vector<float> B{2.f, 3.f, 4.f, 5.f};
	float Ref = 0.f;
	for(size_t I = 0; I < A.size(); ++I)
		Ref += A[I] * B[I];
	const float Sim = AstralDB::Simd::DotProductF32(A.data(), B.data(), A.size());
	REQUIRE(Sim == AstralTest::Approx(Ref).epsilon(0.001));
}

TEST_CASE("examples: every *.sql parses, compiles, runs") {
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
	for(const fs::path &F : Files) {
		const std::string Name = F.filename().string();
		INFO("example: ", Name);
		fs::path Dir = UniqueTempDir("astral_ex_");
		ScopedCwd Cwd(Dir);
		RemoveEphemeralDb(Dir);
		AstralDB::Logger Log((Dir / "ex.log").string(), false);
		const std::string Script = ReadExampleFile(F);
		REQUIRE_MESSAGE(!Script.empty(), Name.c_str());
		AstralDB::SQL::Parser P(Script);
		AstralDB::SQL::Bytecode Code =
		    AstralDB::SQL::BuildBytecode(&Log, AstralDB::SQL::OptimizationLevel::None);
		REQUIRE_MESSAGE(!Code.empty(), Name.c_str());
		AstralDB::SQL::BytecodeInterpreter Interp(&Log);
		REQUIRE_NOTHROW(Interp.Execute(Code));
	}
}
