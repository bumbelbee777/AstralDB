# AstralDB CLI Usage

## Parameters

- `-h`/`--help`: Display help (duh)
- `-v`/`--version`: Do I really have to explain this?
- `-q`/`--query "QUERY"`: Executes provided query and returns.
- `-r`/`--repl`: Runs in query REPL.
- `-c FILE`/`--check FILE`: Only checks input query file, no codegen or execution.
- `-V`/`--verbose`: Enables verbose output.
- `-fb FILE`/`--from-bytecode FILE`: Runs input bytecode.
- `-cc FILE`/`--compile FILE`: Compiles query to bytecode (default output is `out.abc` unless specified with `-o OUT`/`--output OUT`).
- `-Ox`: optimization level, ranges from 0 (no optimizations) to 3 (maximum aggressive optimizations).
- `-l FILE`/`--log-file FILE`: Save logs/audits to specified file.
- `-s FILE`: Evaluates, compiles, and run given query file.
- `-m`/`--mmap`: Use an ephemeral session database file under the OS temp directory (no `astral.db` in the current working directory). Removed when the process exits normally.
- `-U NAME`/`--user NAME`: After the session database is opened, authenticate as `NAME` for the rest of that process (REPL, `-q`, `-s`, `--time-sql`, positional SQL files, `-fb` when the primary file is opened first, and `--export-bundle` / `--import-bundle`). Omit this flag to run without establishing a session user (same as before these options existed).
- `-P SECRET`/`--password SECRET`: Password for the user given by `-U`/`--user`. If you omit `-P`, the CLI reads the **`ASTRALDB_PASSWORD`** environment variable instead (handy for scripts so the secret is not argv-visible). If you authenticate via flags or env, you may supply the username alone with **`ASTRALDB_USER`** instead of `-U` (password rules are unchanged: use `-P` or **`ASTRALDB_PASSWORD`**).

Every new database file seeds a built-in catalog account **`Admin0`** with password **`admin`** when there is no persisted user catalog (first run, or older snapshot files that predate the user trailer). After a **checkpoint snapshot** is written, the file carries the full **`Users_`** list and **ACL** map so reopening the same path restores accounts and grants. Extend accounts through the engine API (`AddUser`, `RemoveUser`, …) until SQL-level user management exists. A failed login does **not** remove or relocate catalog users, and it does **not** replace an already-established session user (the previous session stays active until a different successful `AuthenticateUser` or `Logout`).

Between checkpoints, **`AddUser` / `RemoveUser` / `GrantPermission` / `RevokePermission`** are recorded in **`.wal`** (`UU`, `UD`, `UG`, `UR` lines) alongside table/view operations; replaying a WAL rebuilds the security catalog before the next `SyncToFile` truncates the log.
