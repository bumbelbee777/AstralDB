# AstralDB CLI Usage

Product release **v1.0**. Bytecode container files (`.abc`) use on-disk layout version **1**.

## Parameters

- `-h`/`--help`: Display help (duh)
- `-v`/`--version`: Do I really have to explain this?
- `-q`/`--query "QUERY"`: Executes provided query and returns.
- `-r`/`--repl`: Runs in query REPL.
- `-c FILE`/`--check FILE`: Only checks input query file, no codegen or execution.
- `-V`/`--verbose`: Enables verbose output (includes pretty bytecode disassembly after SQL runs).
- `-fb FILE`/`--from-bytecode FILE`: Runs input bytecode (`.abc` v1 container).
- `-cc FILE`/`--compile FILE`: Compiles query to bytecode (default output is `out.abc` unless specified with `-o OUT`/`--output OUT`).
- `-Ox`: optimization level, ranges from 0 (no optimizations) to 3 (maximum aggressive optimizations).
- `-l FILE`/`--log-file FILE`: Save logs/audits to specified file.
- `-s FILE`: Evaluates, compiles, and run given query file.
- `-m`/`--mmap`: Use an ephemeral session database file under the OS temp directory (no `astral.db` in the current working directory). Removed when the process exits normally.
- `-U NAME`/`--user NAME`: After the session database is opened, authenticate as `NAME` for the rest of that process (REPL, `-q`, `-s`, `--time-sql`, positional SQL files, `-fb` / `--proc-call`, SQL `CALL`, and `--export-bundle` / `--import-bundle`). Omit this flag to run without establishing a session user (same as before these options existed).
- `-P SECRET`/`--password SECRET`: Password for the user given by `-U`/`--user`. If you omit `-P`, the CLI reads the **`ASTRALDB_PASSWORD`** environment variable instead (handy for scripts so the secret is not argv-visible). If you authenticate via flags or env, you may supply the username alone with **`ASTRALDB_USER`** instead of `-U` (password rules are unchanged: use `-P` or **`ASTRALDB_PASSWORD`**).
- `--database PATH`: Use `PATH` as the persistence file (instead of cwd `astral.db` / temp session path).
- `--time-sql FILE`: Run query file; print parse / execute wall times on stderr.
- `--audit-file PATH`: Append security audit events to `PATH` (separate from `-l` debug logging).
- `--export-bundle PATH` / `--import-bundle PATH` with optional `--export-format` / `--import-format` (`csv`|`json`|`tsv`).
- `--convert SRC DST --from FMT --to FMT`: Convert tabular files without opening a full session workflow.

## Bytecode tools (`.abc`)

Compiled programs are **binary v1 containers**: instruction stream plus an optional **string-pool trailer** (when built with `--compile-pool` / `-cp`). Inspection commands do not open a database; execution commands (`-fb`, `--debug-bytecode` / `-dbg`, `--proc-call` / `-px`, SQL `CALL`) use the same session DB rules as SQL (`astral.db`, `-m`, or `--database`).

| Long flag | Shorthand | Purpose |
|-----------|-----------|---------|
| `--inspect-bytecode FILE` | `-ib` | Full summary: instruction counts, DDL/DML flags, tables, opcode histogram. |
| `--bytecode-aspect FILE KIND` | `-ba` | Query one aspect (`stats`, `ddl`, `dml`, `tables`, `opcodes`, `side-effects`, `transactions`, `joins`, `aggregates`, `security`, `all`). |
| `--disasm-bytecode FILE` | `-db` | Pretty disassembly to stdout. |
| `--validate-bytecode FILE` | `-vb` | Structural validation; exit code **2** on errors. |
| `--debug-bytecode FILE` | `-dbg` | Execute with per-instruction VM trace on stderr. |
| `--trace-bytecode` | `-tb` | VM step trace when running `-fb` or `--proc-call` / `-px`. |
| `--debug-steps N` | `-ds` | Stop tracing after `N` steps (`0` = unlimited). |
| `--breakpoint IP` | `-bp` | Repeatable instruction-index breakpoint. |
| `--compile-pool` | `-cp` | With `-cc`: write deduplicated `PUSH_POOL` string trailer. |

## Stored procedures

Procedures map a **name** to a cached **`.abc`** file plus metadata in **`astraldb_procs.json`** (override path with `--proc-catalog` / `-pc`). The catalog tracks:

- **`abc_path`** / **`sql_path`** — on-disk bytecode and source text under **`astraldb_procs_cache/`**
- **`tables`** — table names referenced in the cached bytecode (from inspection)
- **`depends_on`** / **`called_by`** — other procedures invoked via `CALL` / `EXECUTE PROCEDURE` in the source body
- **`abc_index`** — reverse map from `.abc` file to procedure name(s)

### CLI

| Long flag | Shorthand | Purpose |
|-----------|-----------|---------|
| `--proc-register NAME FILE` | `-pr` | Register an existing `.abc` (updates relations). |
| `--proc-unregister NAME` | `-pu` | Remove registration and delete cache files. |
| `--proc-list` | `-pl` | List procedures, `.abc` paths, and `depends_on`. |
| `--proc-info NAME` | `-pi` | Entry metadata + bytecode analysis. |
| `--proc-relations` | `-pg` | Print procedure ↔ `.abc` graph (`abc_index` + dependency edges). |
| `--proc-call NAME` | `-px` | Load and execute the cached `.abc`. |

### SQL (v1.0)

```sql
CREATE PROCEDURE seed AS (
  CREATE TABLE t (id INTEGER);
  INSERT INTO t VALUES (1);
);
CALL seed;
-- EXECUTE PROCEDURE seed;   -- equivalent
DROP PROCEDURE seed;
```

`CREATE PROCEDURE` compiles the parenthesized body, writes **`astraldb_procs_cache/<name>.abc`** and **`.sql`**, updates the catalog, and logs **`PR|`** WAL rows. `CALL` runs the cached bytecode via a nested VM slice. `DROP PROCEDURE` removes catalog entries, cache files, and logs **`PD|`**.

See **`examples/sql_procedure.sql`**.

**Typical workflow**

```text
astraldb -cc examples/hello.sql -o hello.abc
astraldb -ib hello.abc
astraldb -pr seed hello.abc
astraldb -pg
astraldb -px seed
astraldb -s examples/sql_procedure.sql -m
```

## Session database and security

Every new database file seeds a built-in catalog account **`Admin0`** with password **`admin`** when there is no persisted user catalog (first run, or older snapshot files that predate the user trailer). After a **checkpoint snapshot** is written, the file carries the full **`Users_`** list and **ACL** map so reopening the same path restores accounts and grants. Extend accounts through the engine API (`AddUser`, `RemoveUser`, …) until SQL-level user management exists. A failed login does **not** remove or relocate catalog users, and it does **not** replace an already-established session user (the previous session stays active until a different successful `AuthenticateUser` or `Logout`).

Between checkpoints, **`AddUser` / `RemoveUser` / `GrantPermission` / `RevokePermission`** are recorded in **`.wal`** (`UU`, `UD`, `UG`, `UR` lines) alongside table/view/procedure operations; replaying a WAL rebuilds the security catalog before the next `SyncToFile` truncates the log.
