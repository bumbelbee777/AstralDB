# Stored procedures

Procedures map a **name** to cached **`.abc`** bytecode plus metadata in **`astraldb_procs.json`** (override with `--proc-catalog` / `-pc`). Bodies and sources live under **`astraldb_procs_cache/`**.

## Catalog fields

- **`abc_path`** / **`sql_path`** — on-disk bytecode and lowered source
- **`tables`** — table names referenced in bytecode (from inspection)
- **`depends_on`** / **`called_by`** — edges for `CALL` / `EXECUTE PROCEDURE` in bodies
- **`abc_index`** — reverse map from `.abc` file to procedure name(s)
- **`source_dialect`** — `astral`, `plpgsql`, or `plsql` when applicable

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

**PL/pgSQL** (`LANGUAGE plpgsql`, dollar-quoted body) and **PL/SQL** (`IS` / `AS` … `BEGIN` … `END`) are parsed, tagged in the catalog, and **lowered** to sequential SQL before compilation. `CREATE OR REPLACE` overwrites an existing entry. Control-flow blocks (`IF`, loops) are recognized but not lowered yet—keep bodies linear.

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
