# AstralDB CLI usage

Product release **v1.0**. Compiled bytecode containers (`.abc`) use on-disk layout version **1**.

Invoke `astraldb --help` for the same option list the binary prints.

## General options

| Flag | Short | Description |
|------|-------|-------------|
| `--help` | `-h` | Print usage and exit. |
| `--version` | `-v` | Print version and exit. |
| `--query QUERY` | `-q` | Execute one SQL string and exit. |
| `--repl` | `-r` | Interactive SQL REPL. |
| `--check FILE` | `-c` | Parse/check a SQL file only (no codegen or execution). |
| `--verbose` | `-V` | Verbose logging; after SQL runs, pretty-print bytecode disassembly when applicable. |
| `--from-bytecode FILE` | `-fb` | Execute a `.abc` v1 container. |
| `--compile FILE` | `-cc` | Compile SQL to bytecode (default output `out.abc`; use `-o` / `--output` to override). |
| `--output OUT` | `-o` | Output path for `-cc`. |
| `--log-file FILE` | `-l` | Write debug logs to `FILE`. |
| `--audit-file FILE` | | Append security audit events to `FILE` (separate from `-l`). |
| `FILE` | `-s` | Evaluate, compile, and run a SQL file (positional form: `astraldb -s FILE`). |
| `--time-sql FILE` | | Run a SQL file; print parse and execute wall times on stderr. |
| `--mmap` | `-m` | Ephemeral session database under the OS temp directory (removed on normal exit; no `astral.db` in cwd). |
| `--database PATH` | | Persistence file for the session (instead of cwd `astral.db` or the temp path from `-m`). |
| `--user NAME` | `-U` | Authenticate as `NAME` for the process (REPL, `-q`, `-s`, `--time-sql`, positional SQL, `-fb`, `--proc-call`, SQL `CALL`, bundle import/export). |
| `--password SECRET` | `-P` | Password for `-U`. If omitted, read **`ASTRALDB_PASSWORD`**. Username may come from **`ASTRALDB_USER`** when using env-based auth. |
| `--export-bundle PATH` | | Export tabular data (use with `--export-format`). |
| `--import-bundle PATH` | | Import tabular data (use with `--import-format`). |
| `--export-format` / `--import-format` | | `csv`, `json`, or `tsv`. |
| `--convert SRC DST --from FMT --to FMT` | | Convert tabular files without a full SQL session. |
| `-O0` … `-O4` | | Bytecode optimization level: none through maximum (default **`-O1`**). |

## Bytecode tools (`.abc`)

Inspection commands do not open a database. Execution commands (`-fb`, `--debug-bytecode`, `--proc-call`, SQL `CALL`) follow the same session DB rules as SQL (`astral.db`, `-m`, or `--database`).

| Flag | Short | Description |
|------|-------|-------------|
| `--inspect-bytecode FILE` | `-ib` | Summary: instruction counts, DDL/DML flags, tables, opcode histogram. |
| `--bytecode-aspect FILE KIND` | `-ba` | One aspect: `stats`, `ddl`, `dml`, `tables`, `opcodes`, `side-effects`, `transactions`, `joins`, `aggregates`, `security`, `all`. |
| `--disasm-bytecode FILE` | `-db` | Pretty disassembly to stdout. |
| `--validate-bytecode FILE` | `-vb` | Structural validation; exit code **2** on errors. |
| `--debug-bytecode FILE` | `-dbg` | Execute with per-instruction VM trace on stderr. |
| `--trace-bytecode` | `-tb` | Step trace when running `-fb` or `--proc-call`. |
| `--debug-steps N` | `-ds` | Stop tracing after `N` steps (`0` = unlimited). |
| `--breakpoint IP` | `-bp` | Repeatable instruction-index breakpoint. |
| `--compile-pool` | `-cp` | With `-cc`: emit deduplicated `PUSH_POOL` string trailer. |

## Stored procedures (catalog CLI)

| Flag | Short | Description |
|------|-------|-------------|
| `--proc-catalog PATH` | `-pc` | Override `astraldb_procs.json` path. |
| `--proc-register NAME FILE` | `-pr` | Register an existing `.abc` module. |
| `--proc-unregister NAME` | `-pu` | Remove registration and delete cache files. |
| `--proc-list` | `-pl` | List registered procedures. |
| `--proc-info NAME` | `-pi` | Metadata plus bytecode summary for one procedure. |
| `--proc-relations` | `-pg` | Procedure ↔ `.abc` dependency graph. |
| `--proc-call NAME` | `-px` | Load and execute cached bytecode for `NAME`. |

## Triggers (catalog CLI)

| Flag | Short | Description |
|------|-------|-------------|
| `--trig-list` | `-tl` | List triggers (table, timing, event, enabled). |
| `--trig-info NAME` | `-ti` | Full metadata and bytecode summary for one trigger. |
| `--trig-relations` | `-tg` | Table → trigger name index. |
| `--trig-fires` | `-tf` | Recent fire audit log from the catalog. |
