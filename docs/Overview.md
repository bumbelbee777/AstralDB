# AstralDB Overview

AstralDB is a compact relational engine in modern C++ built around **modern SQL**: a growing, test-backed dialect aligned with **SQL-92 core DML/DDL**, **SQL-99 analytics**, and selected **SQL:2003** features—not a full standards certification on the three, but a deliberate path toward readable, executable SQL without a heavyweight runtime.

SQL text is tokenized and parsed into an AST, lowered to **bytecode**, and executed by a small virtual machine that drives tables, indexes, a **write-ahead log**, and optional encryption at rest. The **`astraldb`** CLI runs scripts, ad hoc queries, a simple REPL, and can **compile** queries to **`.abc`** bytecode for later execution or inspection.

> **Experimental:** development is active. Treat AstralDB as **research-grade** until you have audited the code and run your own workloads. **`examples/*.sql`** and **`tests/AstralDB.Tests.cxx`** are the living contract; this document summarizes them and calls out gaps honestly.

---

## Modern SQL: what the dialect is for

AstralDB targets the SQL most teams actually write today: **joins and set operations**, **grouped analytics**, **window functions**, **CTEs (including recursion)**, **upsert-style DML**, and **role-based security**—implemented on a single static binary with predictable bytecode semantics rather than an opaque external planner.

| Layer | Focus | Representative surface |
|-------|--------|-------------------------|
| **SQL-92 core** | Tables, predicates, DML, constraints | `CREATE`/`DROP TABLE`, `INSERT` (multi-row, bulk, `ON CONFLICT`), `UPDATE`/`DELETE`, `SELECT` with `WHERE`/`GROUP BY`/`HAVING`/`ORDER BY`/`LIMIT`/`OFFSET`, joins, `UNION`/`INTERSECT`/`EXCEPT`, `CASE`/`COALESCE`/`CAST`, transactions |
| **SQL-99 analytics** | Reporting and recursion | `WITH` / `WITH RECURSIVE`, window functions (`ROW_NUMBER`, `RANK`, `DENSE_RANK`, `LAG`/`LEAD`, framed aggregates), OLAP (`ROLLUP`/`CUBE`/`GROUPING SETS`, `GROUPING()`), sequences, identity columns, correlated `EXISTS` |
| **SQL:2003+ extensions** | Temporal, semi-structured, search | `FOR SYSTEM TIME AS OF`, `MATCH_RECOGNIZE`, JSON/XML scalars, full-text `MATCH`, vector indexes, advanced cell types (`STRUCT`, `MAP`, `LIST`, `VECTOR`, …) |
| **Graph / GQL** | Table-backed graphs + adjacency index | `CREATE GRAPH`, projections, `GRAPH MATCH` (incl. `*1..n`), `SHORTEST PATH`, `PAGERANK`, `TRAVERSE` — [`GraphGql.md`](GraphGql.md), `examples/graph_*.sql` |
| **Operational SQL** | Durability and governance | RBAC (`GRANT`/`REVOKE`, roles), row/column grants, views, stored procedures (`.abc` cache), `EXPORT`/`IMPORT` bundles |

Runnable scripts for each band live under **`examples/`** (for example **`sql92_03_coverage.sql`**, **`sql92_analytics.sql`**, **`sql99_recursive.sql`**, **`sql_olap.sql`**, **`sql_window_frames.sql`**, **`sql_merge_upsert.sql`**, **`sql_standard_features.sql`**). When this overview and the scripts disagree, **trust the scripts and tests**.

---

## SQL-92 core

**Queries and predicates.** `SELECT` with column lists (including `CAST`, searched `CASE`, `COALESCE`, `NULLIF`, `GREATEST`/`LEAST`), `DISTINCT`, comma-`FROM` (chained `CROSS JOIN` before explicit joins), `WHERE`, `ORDER BY`, `LIMIT`/`OFFSET` (including `FETCH FIRST` and comma `LIMIT offset, count`). Predicates support comparisons, `AND`/`OR`/`NOT`, `IS [NOT] NULL`, `IN`/`NOT IN`, `LIKE`, `BETWEEN`, `TRUE`/`FALSE`, and **`EXISTS` / `NOT EXISTS`** (including correlation via merged outer/inner rows).

**Joins (experimental).** `INNER` / `LEFT` / `RIGHT` / `FULL` / `CROSS JOIN` with `ON` equality (and `AND`); duplicate column names resolve with **left preference**. Results land in scratch tables such as **`__AstralJoin_0`**.

**Grouping.** `GROUP BY` on plain or `table.column` keys (grouping uses the **final column name** after `.`). Aggregates include `COUNT(*)`, `COUNT(DISTINCT col)`, `SUM`/`MIN`/`MAX`/`AVG` with optional `AS`. **`HAVING`** is lowered only for aggregates that appear in the `SELECT` list (see caveats).

**Set operations.** `UNION` / `UNION ALL` / `INTERSECT` / `EXCEPT` (with `ALL` multiset variants where implemented).

**DML and constraints.** Multi-row and **bulk** `INSERT`, restricted **`MERGE INTO … USING … ON …`**, `UPDATE`/`DELETE`, `CREATE`/`DROP TABLE`, `PRIMARY KEY`, **`FOREIGN KEY`** (including composite keys, `ON DELETE` actions, WAL + snapshot persistence), **`CHECK`** (DNF-packed at DDL; enforced on `INSERT`/`UPDATE`), `CREATE VIEW`, and basic **`GRANT` / `REVOKE`**.

**Scalars and strings.** SQL-99 string/datetime builtins (`SUBSTRING … FROM …`, `POSITION … IN …`, `TRIM`, `EXTRACT`, `DATE_ADD`/`DATE_SUB`/`DATE_DIFF`, plus time-series helpers documented with **`TimeSeries.*`**).

---

## SQL-99 analytics

**Common table expressions.** `WITH [RECURSIVE] name [(cols…)] AS ( … )` with comma-separated definitions. Non-recursive bodies are a single `SELECT`. **`WITH RECURSIVE`** uses anchor `UNION ALL` recursive arms; the VM runs a **`RECURSIVE_CTE_FIXPOINT`** loop (bounded by **`Limits::MaxCteRecursionDepth`**) that appends only new row signatures each iteration.

**Windows.** `ROW_NUMBER()` / `RANK()` / `DENSE_RANK()`, `SUM`/`MIN`/`MAX`/`AVG(col)` and `LAG`/`LEAD` with **`OVER ( [ PARTITION BY … ] ORDER BY … [ frame ] )`**. Default running frames use **`ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`**; explicit `ROWS BETWEEN` bounds are supported. **`RANGE` / `GROUPS` frames are not implemented.**

**OLAP.** `GROUP BY … WITH ROLLUP`, `WITH CUBE` (capped at **8** key columns), and `GROUPING SETS ((…), …)`. Rolled-up dimensions appear as **empty strings**; metadata columns **`_olap_level`** / grouping helpers support `GROUPING(col)` and **`GROUPING_ID(…)`**. **`COUNT(DISTINCT)` cannot be combined with ROLLUP/CUBE/GROUPING SETS yet.**

**Sequences and identity.** `CREATE SEQUENCE`, `NEXTVAL(seq)`, and **`GENERATED { ALWAYS | BY DEFAULT } AS IDENTITY`** columns with optional `(START WITH … INCREMENT BY …)`.

---

## SQL:2003 and extended types

**Temporal.** `FOR SYSTEM TIME AS OF <timestamp>` and `FROM t AS OF '…'` filter rows using optional **`valid_from` / `valid_to`** columns (epoch seconds or values understood by **`TimeSeries::ParseEpochSeconds`**). Tables without those columns yield an empty snapshot under `AS OF`.

**Row patterns.** `MATCH_RECOGNIZE` with `ORDER BY`, `PATTERN ( … )`, and `DEFINE` predicates (DNF-backed, same subset as `WHERE`).

**JSON, XML, search.** `JSON_EXTRACT`, `JSON_CONTAINS`, `JSON_MERGE`, and related scalars; XML extract/serialize/validate; **`WHERE col MATCH '…'`** with optional FTS indexes; **`CREATE INDEX … USING VECTOR`** and **`VECTOR_TOPK`**.

**Advanced cells and MathSci.** DuckDB-style **`STRUCT`**, **`MAP`**, **`LIST`**, **`VECTOR`**, **`MATRIX`**, **`COMPLEX`**; geospatial and dataset opcodes; SIMD signal/FFT, autograd, and differential-equation solvers—see **`examples/sql_math_sci.sql`**, **`docs/MathSciSignal.md`**, **`docs/MathSciAutograd.md`**, **`docs/MathSciSolves.md`**.

---

## Caveats (read before relying on semantics)

These limits are intentional guardrails until the grammar and VM catch up. If your workload hits one of them, add an **`examples/`** regression or extend the compiler—do not assume “standard SQL” behavior by default.

| Area | Limitation |
|------|------------|
| **Identifiers** | Reserved words are **ASCII case-insensitive**; unquoted names that collide with keywords (e.g. `RANK`) parse as keywords—use safer names until quoted identifiers exist. |
| **`SELECT *`** | Allowed in simple `SELECT` arms; **rejected inside** `UNION` / `INTERSECT` / `EXCEPT`—name columns explicitly. |
| **`HAVING`** | Only with **`GROUP BY`**, and only for aggregates **already present** in the `SELECT` list (or `COUNT(*)` / `COUNT(DISTINCT)` shapes the compiler recognizes). Arbitrary `HAVING` expressions fail at bytecode build. |
| **`NULL`** | Missing cells and empty strings interact closely in storage; `IS NULL` follows VM **`CellIsSqlNull`** rules—validate edge cases for your data. |
| **Transactions** | `BEGIN` snapshots the primary DB file; `ROLLBACK` restores that copy and clears adjacent **`.wal`** sidecars. Large databases and long transactions are bounded by whole-file copy limits. |
| **`CAST` date/time** | Type names parse, but conversion is **string coercion only**—no timezone-aware calendar math. |
| **`COUNT`** | `COUNT(*)`, optional `AS`, and **`COUNT(DISTINCT one_column)`** only—no `COUNT(ALL …)`, no multi-column `DISTINCT`, no arbitrary expressions inside `COUNT`. |
| **`MERGE` / upsert** | `MERGE` needs **`target.col = source.col`**-style `ON` keys (composite allowed); `SET`/`INSERT` values are literals, alias columns, `EXCLUDED.*`, or simple `+ - * /` over those. `INSERT … ON CONFLICT` uses PK or an explicit column list. |
| **`CASE` / `COALESCE` in SELECT** | `THEN`/`ELSE` scalars are literals, plain columns, or `NULL` in the documented subset—no nested `CASE` or arithmetic in `THEN` yet. |
| **Joins / OLAP** | Join subset is **experimental**; OLAP rolled-up cells are **empty strings**, not SQL `NULL`. |
| **Optimizer** | Levels **`-O0` … `-O4`** run bytecode passes (peephole, constant fold, DCE, and extra rounds at **`-O4` Maximum**). Invalid control flow **reverts** the pre-pass bytecode. The stack VM does **not** use classical register allocation. |
| **Security** | Unauthenticated sessions skip ACL checks (legacy scripts). Default **`Admin0`** credentials are **development-only**. |

---

## How a query runs

1. **Tokenizer → parser → AST** (`sources/SQL/Parser.cxx`, `AST` in `SQL.hxx`).
2. **Codegen → bytecode** (`BuildBytecode` in `Codegen.cxx`).
3. **Optimizer pipeline** (`RunOptimizerPipeline` in `Optimizer.cxx`), selected by **`-O0` (none)** through **`-O4` (maximum)** on the CLI.
4. **BytecodeInterpreter** executes opcodes against **`Database`** (tables, WAL, indexes, optional hybrid columnar paths).

**Bytecode tooling (v1.0 `.abc`).** Compile with **`-cc`**, inspect/disassemble/validate with **`-ib` / `-db` / `-vb`**, debug with **`-dbg` / `-tb` / `-bp`**. Stored procedures compile to **`astraldb_procs_cache/<name>.abc`** with catalog **`astraldb_procs.json`**—see [`Usage.md`](Usage.md) and **`examples/sql_procedure.sql`**.

**Build quality.** **`ASTRALDB_WARNINGS_AS_ERRORS`** defaults to **ON**; shared **`SafeDiv()`** in **`sources/IO/MathUtil.hxx`** guards analytics kernels.

---

## Storage, durability, and security (intent)

**Durability.** In-memory tables with a **WAL** (`W1|` lines: FEC + **XChaCha20** under **`kAtRestXChaKey`**, with legacy plaintext replay). Periodic **`SyncToFile`** encrypts the main snapshot and truncates the WAL. Spinlock-backed concurrency and bounded **`std::async`** work guard hot paths.

**Sessions and RBAC.** A catalog of users (seed **`Admin0` / `admin`** on first open), optional authenticated session (**`-U` / `-P`** or env vars), table ACLs unioned with **role** grants, plus **row-** and **column-level** fine grants. DDL and privilege changes require **ALL** on the session user. Snapshots and WAL replay restore users, ACLs, foreign keys, views, and sequences—see **`examples/security_rbac.sql`**.

**Threat model.** Design direction is **best-effort** encryption and auditability, not a certified product. Read the paths you depend on; prefer **`ASTRALDB_PASSWORD`** over argv passwords for automation.

---

## Analytics patterns (quick recipes)

Patterns that map cleanly to today’s bytecode (see **`examples/sql92_analytics.sql`**):

1. **Distinct per bucket:** `SELECT key, COUNT(DISTINCT visitor_id) FROM events GROUP BY key`.
2. **Fallback scalar:** `COALESCE(priority_col, backup_col, '0')` (literals/columns only—or explicit `CASE` when you need more).
3. **Partition ranking:** `ROW_NUMBER() OVER (PARTITION BY dept ORDER BY score DESC) AS rn`—filter top-N via a follow-up statement or materialized scratch table today.
4. **Ties:** `RANK()` vs `DENSE_RANK()` vs unique `ROW_NUMBER()`—pick the function that matches your reporting rules.

---

## Vision and performance posture

The long-term goal is a **fast, small, readable** RDBMS: performance from tight structures, deliberate concurrency, and a clear split between **language front end**, **bytecode**, and **storage**—not from opaque dependencies.

**SQLite** is a **directional benchmark**, not a claim of dominance. Tune with fixed scripts under **`examples/`** (bulk loads, join benches) and compare on the **same synthetic batch** with **`astraldb --time-sql`** and, when useful, `sqlite3` on the same machine.

Near-term work: widen SQL coverage, harden semantics (especially `NULL` and optimizer safety), and keep operator knobs (logging, session DB paths, bytecode I/O) predictable.

---

## Where to look next

| Topic | Location |
|-------|----------|
| CLI flags and examples | [`Usage.md`](Usage.md) |
| Graph / GQL | [`GraphGql.md`](GraphGql.md) |
| C++ conventions | [`CodingStyle.md`](CodingStyle.md) |
| Cluster orchestration | [`Quasar.md`](Quasar.md) |
| Runnable contract | `examples/`, `tests/` |
| Build | Root [`README.md`](../README.md) (InsurgeNT / CMake / CI) |
