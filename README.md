# AstralDB

[![CI](https://github.com/bumbelbee777/AstralDB/actions/workflows/ci.yml/badge.svg)](https://github.com/bumbelbee777/AstralDB/actions/workflows/ci.yml)

**Humanity's last RDBMS** — a compact, high-performance relational engine in modern C++: SQL front end, bytecode VM, WAL-backed storage, and a **single static executable** with **no runtime dependencies**.

> **Still under active development.** Treat as experimental until you have audited it yourself; not for production yet.

## One binary, the full stack

| | |
|---|---|
| **Release `astraldb.exe`** | **~1.5 MB** (Windows Release; CMake `astraldb_cli` ~2.0 MB) |
| **Core sources** | ~27k lines across ~85 source files |
| **Contract tests** | **103+** doctest cases |
| **Example SQL harness** | **31+** `examples/*.sql` scripts (parse → compile → run in CI) |

No server process, no JVM, no opaque planner DLLs. One CLI, one database file (or ephemeral `-m` session). Inside that footprint is:

- **SQL-92/99/2003 analytics:** joins, OLAP (`ROLLUP`/`CUBE`/`GROUPING SETS`), CTEs (`WITH RECURSIVE`), windows, `EXISTS`, `MERGE`/upsert, transactions
- **Advanced types:** `STRUCT`, `MAP`, `VECTOR`, `MATRIX`, `COMPLEX`, `LIST`, **`POINT`**, **`VARIANT`**, **`TERRAIN`**
- **Geospatial:** `ST_POINT`, `ST_POINTZ`, `ST_ELEVATION`, `ST_DEM_SAMPLE`, `ST_TERRAIN_SLOPE`, distances, WKT interop
- **Maintenance:** `VACUUM` / `VACUUM TABLE`, `REPACK TABLE … CONCURRENTLY`
- **Time series:** `TIME_BUCKET`, `DATE_TRUNC`, **`TS_COMPRESS` / `TS_DECOMPRESS`** delta codecs
- **MathSci:** SIMD signal/FFT, autograd, **configurable ODE/SDE/PDE solvers** (`SOLVE_ODE`, Heun, implicit Euler, Milstein, advection, wave)!
- **Datasets:** `CREATE DATASET … AS TABLE|BULK`, versioned snapshots, `LOAD DATASET … VERSION n INTO`
- **Bytecode tooling:** `.abc` compile/inspect/debug, stored procedures
- **Durability & security:** encrypted WAL, RBAC, row/column grants, hybrid row/columnar storage, FTS & vector indexes

See [`docs/Overview.md`](docs/Overview.md) for the full contract.

## Quasar (production orchestration)

[**Quasar**](docs/Quasar.md) (v1.0) wraps the same `astraldb` executable for **multi-file deployments**: **sharding**, **pooled/batched** high-QPS access, **cross-shard transactions**, **multi-region** replication, multi-master, failover, rebalancing, cross-shard JOIN, and an optional **HTTP gateway**.

```bash
pip install -r quasar/requirements.txt
export QUASAR_ASTRALDB=build-ci/astraldb_cli    # Windows: astraldb_cli.exe

python -m quasar init ./cluster
python -m quasar health ./cluster/cluster.json
python -m quasar shard ./cluster/cluster.json "SELECT 1;" --shard-key tenant:acme
python -m quasar backup-all ./cluster/cluster.json
python -m quasar status ./cluster/cluster.json
python -m quasar drift ./cluster/cluster.json
python -m quasar batch ./cluster/cluster.json quasar/examples/batch.example.sql
```

Quasar batches many queries into shared `astraldb` subprocesses by default (`pool.enabled`). It does **not** add a network server inside the engine. See [`docs/Quasar.md`](docs/Quasar.md) for cross-shard transactions, multi-region, and **caveats** (best-effort 2PC, async geo-replication).

## Memory guards (spike-aware, near-zero steady-state cost)

`MemoryGuard` stays **disabled** during normal work and arms only on **spikes** (single allocation ≥ 8 MB, ≥ 32 MB growth in 100 ms, or inside explicit bulk/dataset/maintenance paths). When active, session growth is capped at **512 MB** with **bounds-checked** index/size pairs. Bulk insert, `LOAD DATASET`, `VACUUM`, and `REPACK` always run under a `SpikeScope`.

## Benchmark vs other engines

On an **SQL-92 analytic** workload (CTE + JOIN + GROUP BY + window) at roughly **1k customers / 10k orders / 50k line items** (0.001 scale), median wall time on the same machine:

![SQL-92 analytic benchmark: AstralDB vs DuckDB vs SQLite](media/astraldb_bench.png)

| Engine | Median wall time |
|--------|------------------|
| **AstralDB** | **13.9 ms** |
| DuckDB | 19.0 ms |
| SQLite 3.49.1 | 116.7 ms |

Reproduce with `scripts/benchmark_torture_plot.py` and `examples/benchmarks/benchmark_torture_unified.sql`.

## Stress & torture suites

Native **`INSERT_BULK`** (one VM opcode, one storage pass) powers CI and off-CI harnesses. Median wall time across **8 suites**, **3 runs each**, `-O2`, isolated temp cwd (same machine as the cross-engine bench above):

![AstralDB stress & torture suites: median wall time per harness](media/stress_torture_histogram.png)

| Suite | Scale | Median wall time |
|-------|------:|-----------------:|
| [`torture_test.sql`](examples/torture_test.sql) | 6k bulk + DML/WAL | **15.5 ms** |
| [`torture_advanced.sql`](examples/torture_advanced.sql) | geo + MathSci + datasets | **16.8 ms** |
| [`torture_unhinged.sql`](examples/torture_unhinged.sql) | 16k bulk + `VACUUM` / `REPACK` | **15.6 ms** |
| [`stress_traffic.sql`](examples/stress_traffic.sql) | 16k rows, multi-commit | **21.1 ms** |
| [`benchmark_torture.sql`](examples/benchmark_torture.sql) | joins + aggregates | **17.9 ms** |
| [`stress_bulk_load.sql`](examples/benchmarks/stress_bulk_load.sql) | **150k** bulk | **17.5 ms** |
| [`stress_wal_churn.sql`](examples/benchmarks/stress_wal_churn.sql) | 35k WAL + DML churn | **18.6 ms** |
| [`stress_analytics_mix.sql`](examples/benchmarks/stress_analytics_mix.sql) | 8k / 32k join | **14.0 ms** |

Reproduce the chart with `scripts/stress_torture_histogram.py` (or run suites individually via `scripts/run_stress_suite.ps1 -All`).

```powershell
pip install -r scripts/benchmark-requirements.txt
python scripts/stress_torture_histogram.py --runs 3 --output media/stress_torture_histogram.png
```

## Datasets (explicit named fixtures)

```sql
CREATE TABLE src (id INT, a INT, b TEXT, c TEXT, d TEXT);
INSERT INTO src BULK 1000 START 1 STEP 1;
CREATE DATASET snap AS TABLE src;          -- version 1 snapshot (5 rows)
INSERT INTO src BULK 10 START 1 STEP 1;
CREATE DATASET snap AS TABLE src;          -- version 2 snapshot (10 rows)

CREATE TABLE dst (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET snap VERSION 1 INTO dst;      -- point-in-time load
LOAD DATASET bench INTO gen;               -- synthetic bulk fixture
```

See [`examples/sql_dataset.sql`](examples/sql_dataset.sql) and [`examples/sql_dataset_versioning.sql`](examples/sql_dataset_versioning.sql).

## Build

```bash
pip install insurgent && cd AstralDB && insurgent build
```

CMake (matches [GitHub Actions CI](.github/workflows/ci.yml)):

```bash
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release
cmake --build build-ci
ctest --test-dir build-ci --output-on-failure
```

On Linux/macOS you can also run `bash scripts/ci/build_and_test.sh`.

**CI** (every push/PR to `main`): GCC and Clang on Ubuntu 24.04, Clang on Windows — full `run_tests` plus a `torture_test.sql` CLI smoke check. **Releases**: pushing a `v*` tag builds `astraldb` / `astraldb.exe` artifacts via [`.github/workflows/release.yml`](.github/workflows/release.yml).
