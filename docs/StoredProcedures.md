# Stored procedures

Procedures map a **name** to cached **`.abc`** bytecode plus metadata in **`astraldb_procs.json`** (override with `--proc-catalog` / `-pc`). Bodies and sources live under **`astraldb_procs_cache/`**.

## Catalog fields

- **`abc_path`** / **`sql_path`** — on-disk bytecode and lowered source
- **`tables`** — table names referenced in bytecode (from inspection)
- **`depends_on`** / **`called_by`** — edges for `CALL` / `EXECUTE PROCEDURE` in bodies
- **`abc_index`** — reverse map from `.abc` file to procedure name(s)
- **`source_dialect`** — `astral`, `plpgsql`, `plsql`, or `tsql` when applicable

## SQL surface

**AstralDB** (parenthesized statement list):

```sql
CREATE PROCEDURE seed AS (
  CREATE TABLE t (id INTEGER);
  INSERT INTO t VALUES (1);
);
CALL seed;
EXEC seed;
EXECUTE seed;
EXECUTE PROCEDURE seed;
DROP PROCEDURE seed;
```

**PL/pgSQL** (`LANGUAGE plpgsql`, dollar-quoted body), **PL/SQL** (`IS` / `AS` … `BEGIN` … `END`), and **T-SQL** (`CREATE [OR ALTER] PROCEDURE` / `PROC`, `@parameters`, `AS` … `BEGIN` … `END`, `BEGIN TRY` / `BEGIN CATCH`) are parsed, tagged in the catalog, and **lowered** to sequential SQL before compilation. `CREATE OR REPLACE` (and T-SQL `CREATE OR ALTER`) overwrites an existing entry.

T-SQL lowering (subset): `IF` / `ELSE IF` / `WHILE` blocks with `BEGIN`/`END` map to PL/SQL-style `THEN` / `ELSIF` / `END IF` / `LOOP`; `SET NOCOUNT`, `PRINT`, `RETURN`, and `DECLARE @…` are stripped; string-literal `EXEC(…)` is inlined like `EXECUTE IMMEDIATE`.

Control-flow lowering (PL/SQL and PL/pgSQL bodies):
- **Constant** `IF` / `ELSIF` / `ELSE` / `END IF` — folded at lower time when every branch condition is a compile-time constant (`TRUE`, `FALSE`, `1=1`, `2<>3`, `NOT FALSE`, …).
- **Runtime** `IF` / `ELSIF` / `ELSE` / `END IF` and searched `CASE` / `CASE … END CASE` — preserved as structured control segments, serialized in procedure metadata, and compiled to bytecode branches (`PROC_JUMP_IF_TABLE_EMPTY` after a probe `SELECT` per condition). Use predicates the SQL engine can codegen (for example `EXISTS (SELECT 1 FROM t WHERE …)` or column comparisons), not arbitrary expressions wrapped only in parentheses on `DUAL`.
- `WHILE` loops when the condition is constant (`WHILE TRUE` unwraps the body; `WHILE FALSE` removes it).
- `FOR i IN 1..N LOOP` when `N` is a literal and the iteration count is at most 32 (body is unrolled).
- `EXCEPTION WHEN … THEN` handlers are split for bytecode exception stitching (see catalog `source_dialect`).
- Non-constant `WHILE` / `FOR` loops are still rejected with a clear lowering error.

Identifier names such as `if_probe` are not treated as the `IF` keyword (word boundaries include `_`).

## C API (`include/astraldb/AstralDB.h`)

- `AstralDbVersion`, `AstralDbCall`, `AstralDbExecQuery` (row callback per cell).
- Prepared statements: `AstralDbPrepare` validates SQL; the first `AstralDbStmtStep` runs it with any `AstralDbBindInt64` / `AstralDbBindText` bindings (`?` is 1-based). `AstralDbStmtReset` clears the loaded result so you can re-bind and step again.

`CREATE PROCEDURE` compiles the body, writes cache files, updates the catalog, and logs **`PR|`** WAL rows. `CALL` runs cached bytecode in a nested VM slice. `DROP PROCEDURE` removes catalog entries, cache files, and logs **`PD|`**.

## CLI workflow

```text
astraldb -cc examples/hello.sql -o hello.abc
astraldb -ib hello.abc
astraldb -pr seed hello.abc -m
astraldb -pl
astraldb -px seed
astraldb -s examples/sql_procedure.sql -m
```

CLI flags are listed in [`Usage.md`](Usage.md).

## Examples

- [`examples/sql_procedure.sql`](../examples/sql_procedure.sql)
- [`examples/sql_procedure_plpgsql.sql`](../examples/sql_procedure_plpgsql.sql)
- [`examples/sql_procedure_plsql.sql`](../examples/sql_procedure_plsql.sql)
- [`examples/sql_procedure_tsql.sql`](../examples/sql_procedure_tsql.sql)
