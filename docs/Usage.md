# AstralDB CLI Usage

## Parameters

- `-h`/`--help`: Display help (duh)
- `-v`/`--version`: Do I really have to explain this?
- `-q`/`--query "QUERY"`: Executes provided query and returns.
- `-r`/`--repl`: Runs in query REPL.
- `-c FILE`/`--check FILE`: Only checks input query file, no codegen or execution.
- `-V`/`--verbose`: Enables verbose output.
- `-fb FILE`/`--from-bytecode FILE`: Runs input bytecode.
- `-cc FILE`/`--compile FILE`: Compiles query to bytecode (default output is `out.abc` unless specified with `-o OUT`).
- `-l FILE`/`--log-file FILE`: Save logs/audits to specified file.
- `-s FILE`: Evaluates, compiles, and run given query file.
- `-m`/`--mmap`: Only store database in memory and not on the disk.