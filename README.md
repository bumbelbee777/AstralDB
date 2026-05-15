# AstralDB

A compact, high-performance relational database engine in modern C++—SQL front end, bytecode VM, WAL-backed storage, and a single CLI with no runtime dependencies.

> **Still under heavy development.** Treat as experimental until you have audited it yourself—not for production yet.

## Benchmark

On an **SQL-92 analytic** workload (CTE + JOIN + GROUP BY + window) at roughly **1k customers / 10k orders / 50k line items** (0.001 scale), median wall time on the same machine:

![SQL-92 analytic benchmark: AstralDB vs DuckDB vs SQLite](media/astraldb_bench.png)

| Engine | Median wall time |
|--------|------------------|
| **AstralDB** | **13.9 ms** |
| DuckDB | 19.0 ms |
| SQLite 3.49.1 | 116.7 ms |

**AstralDB finishes first** on this run—about **1.4× faster than DuckDB** and roughly **8× faster than SQLite** for the same script. Numbers vary by hardware and build; reproduce with `astraldb --time-sql` and scripts under `examples/`.

## What you get

- **Growing SQL surface:** joins, `GROUP BY`, CTEs (`WITH`), window functions, `EXISTS`, `CASE`, `CAST`, `COALESCE`, transactions—see [`docs/Overview.md`](docs/Overview.md) and `examples/*.sql`.
- **Bytecode pipeline:** parse → AST → optimize (`-O0`…`-O3`) → VM.
- **Durability:** WAL, snapshots, optional at-rest encryption (XChaCha20).
- **Security:** users, ACLs, RBAC, row/column grants, audit log (`--audit-file`).

## Why it is fast

A small codebase: predictable structures, bounded async work, prefetch-friendly scans, and optimizer passes before execution—not a large dependency stack.

## Build

AstralDB uses the InsurgeNT build system to compile and run.

```bash
pip install insurgent
cd AstralDB
insurgent build
```

In case InsurgeNT fails, CMake it also supported:

```bash
cd AstralDB
cmake -B build-cmake && cmake --build build-cmake --config Release
```