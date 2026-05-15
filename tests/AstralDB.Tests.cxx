/* Database before doctest so schema types are parsed before short macro names leak in. */
#include <Database/Database.hxx>
#include <IO/Logger.hxx>
#include <SQL/SQL.hxx>
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include "AstralTestHelpers.hxx"
#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <system_error>

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
