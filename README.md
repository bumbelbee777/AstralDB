# AstralDB 2.0

[![CI](https://github.com/bumbelbee777/AstralDB/actions/workflows/ci.yml/badge.svg)](https://github.com/bumbelbee777/AstralDB/actions/workflows/ci.yml)
[![Nightly](https://github.com/bumbelbee777/AstralDB/actions/workflows/nightly.yml/badge.svg)](https://github.com/bumbelbee777/AstralDB/actions/workflows/nightly.yml)

**Humanity's last RDBMS** — a compact, high-performance relational engine in modern C++: SQL front end, bytecode VM, WAL-backed storage, and a **single static executable** with **no runtime dependencies**.

**AstralDB 2.0** adds MathSci inference (MCTS, Bayesian updates, neural macro Fokker–Planck), expanded PINN and autograd tooling, spike-aware memory guards, and **Quasar 2.0** for sharded production orchestration—still one `astraldb` binary under the hood.

## Quick start

**Download** a [release](https://github.com/bumbelbee777/AstralDB/releases) (`v2.0`) or [nightly](https://github.com/bumbelbee777/AstralDB/releases/tag/nightly) binary (verify with `sha256sum -c SHA256SUMS`), or build from source:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target astraldb
./build/astraldb -m "CREATE TABLE t (id INT); INSERT INTO t VALUES (1); SELECT * FROM t;"
```

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target astraldb
.\build\Release\astraldb.exe -m "CREATE TABLE t (id INT); INSERT INTO t VALUES (1); SELECT * FROM t;"
```

Run a script: `astraldb -f examples/sql92_analytics.sql`. Inspect bytecode: `astraldb -c "SELECT 1" -o query.abc`.

Full CLI flags: `astraldb --help`. C API: [`include/astraldb/AstralDB.h`](include/astraldb/AstralDB.h).

## At a glance

| | |
|---|---|
| **Release binary** | **~1.5 MB** with default `-DASTRALDB_RELEASE_PROFILE=size` (CI/release); larger without size tuning (e.g. ~2.5 MB MSVC Release) |
| **Engine sources** | **46,285** lines in **132** `.cxx`/`.hxx` files under `sources/` |
| **Contract tests** | **170** doctest cases (**1171** assertions; perf tier: `run_tests --test-suite=perf`) |
| **Example SQL** | **63** scripts (**55** under `examples/`, **8** under `examples/benchmarks/`) |
| **Version** | `astraldb --version` → **2.0** (`.abc` container layout **v1**) |

No server process, no JVM, no opaque planner DLLs. One CLI, one database file (or ephemeral `-m` session).

## What's new in 2.0

- **MathSci inference** — `MCTS_SEARCH`, conjugate/grid Bayesian updates, `NFP_MACRO_MARCH` (HANK-style macro Fokker–Planck); see [`docs/MathSciInference.md`](docs/MathSciInference.md), [`docs/MathSciFokkerPlanck.md`](docs/MathSciFokkerPlanck.md)
- **PINN & models in SQL** — `PREDICT`, `MSE_LOSS`, `PINN_FD_CENTRAL`, binary `MATHSCI_MODEL_*` cells with compiled MLP cache and SIMD `matvec`; see [`docs/MathSciPinn.md`](docs/MathSciPinn.md), [`docs/MathSciModel.md`](docs/MathSciModel.md)
- **Memory guards** — near-zero steady-state cost; arms on allocation spikes and bulk/maintenance paths (`LOAD DATASET`, `VACUUM`, `REPACK`)
- **Quasar 2.0** — sharding, pooled access, cross-shard transactions, multi-region replication, HTTP gateway; see [`docs/Quasar.md`](docs/Quasar.md)
- **Broader contract** — more examples, torture suites, and doctest coverage across geospatial, datasets, GQL, and dialect compat

> Mission-critical deployments should still run your own audits and workloads. See caveats in [`docs/Overview.md`](docs/Overview.md).

## Capabilities

Inside the single binary:

- **SQL-92/99/2003 analytics** — joins (subset **experimental**), OLAP (`ROLLUP`/`CUBE`/`GROUPING SETS`), CTEs (`WITH RECURSIVE`), windows, `EXISTS`, `MERGE`/upsert, transactions
- **Advanced types** — `STRUCT`, `MAP`, `VECTOR`, `MATRIX`, `COMPLEX`, `LIST`, **`POINT`**, **`VARIANT`**, **`TERRAIN`**
- **Geospatial** — 2D `POINT`/`POLYGON`, 3D `MESH` (glTF, CSG), terrain DEM — [`docs/Geospatial.md`](docs/Geospatial.md)
- **Graph / GQL** — `CREATE GRAPH`, `GRAPH MATCH`, shortest path, PageRank — [`docs/GraphGql.md`](docs/GraphGql.md)
- **Time series** — `TIME_BUCKET`, `DATE_TRUNC`, **`TS_COMPRESS` / `TS_DECOMPRESS`**
- **MathSci** — SIMD signal/FFT, autograd, ODE/SDE/PDE solvers, classifiers, NLP, embeddings, tiny LM training — [`docs/MathSciLmTrain.md`](docs/MathSciLmTrain.md)
- **Datasets** — `CREATE DATASET … AS TABLE|BULK`, versioned snapshots, `LOAD DATASET … VERSION n INTO`
- **Bytecode** — `.abc` compile/inspect/debug, stored procedures, triggers
- **Durability & security** — encrypted WAL, RBAC, row/column grants, hybrid row/columnar storage, FTS & vector indexes
- **Maintenance** — `VACUUM` / `VACUUM TABLE`, `REPACK TABLE … CONCURRENTLY`

See [`docs/Overview.md`](docs/Overview.md) for the full contract and honest limits.

## Quasar (production orchestration)

[**Quasar**](docs/Quasar.md) wraps the same `astraldb` executable for **multi-file deployments**: sharding, pooled/batched access, cross-shard transactions, multi-region replication, failover, rebalancing, cross-shard JOIN, and an optional HTTP gateway. It does **not** embed a network server in the engine.

```bash
pip install -e ".[dev]"
export QUASAR_ASTRALDB=build/astraldb

python -m quasar init ./cluster
python -m quasar health ./cluster/cluster.json
python -m quasar shard ./cluster/cluster.json "SELECT 1;" --shard-key tenant:acme
```

```powershell
pip install -e ".[dev]"
$env:QUASAR_ASTRALDB = "build\Release\astraldb.exe"

python -m quasar init .\cluster
python -m quasar health .\cluster\cluster.json
```

Set `QUASAR_ASTRALDB` to your built CLI. Config paths use forward slashes; Quasar resolves them under the config directory on all platforms.

## Performance

Benchmarks are **median wall time**, **3 runs**, on reference hardware with **Release** + default **`ASTRALDB_RELEASE_PROFILE=size`** (see [`cmake/AstralDBCompileOptions.cmake`](cmake/AstralDBCompileOptions.cmake)). Medians vary by CPU, OS, and profile; regenerate before tagging via [`docs/RELEASING.md`](docs/RELEASING.md) (install DuckDB/SQLite on PATH for the cross-engine chart—do not use `--skip-duckdb` / `--skip-sqlite` when refreshing `media/astraldb_bench.png`).

### SQL-92 analytic vs DuckDB and SQLite

CTE + JOIN + GROUP BY + window at ~1k customers / 10k orders / 50k line items (`--scale 0.001`):

![SQL-92 analytic benchmark: AstralDB vs DuckDB vs SQLite](media/astraldb_bench.png)

| Engine | Median |
|--------|-------:|
| **AstralDB** | **15.5 ms** |
| DuckDB | 16.5 ms |
| SQLite 3.49.1 | 116.4 ms |

```bash
python scripts/benchmark_torture_plot.py --astral build/astraldb --runs 3 --output media/astraldb_bench.png
```

Query: [`examples/benchmarks/benchmark_torture_unified.sql`](examples/benchmarks/benchmark_torture_unified.sql).

### Stress & torture suites

Native **`INSERT_BULK`** (one VM opcode, one storage pass) across eight harnesses:

![Stress & torture suites](media/stress_torture_histogram.png)

| Suite | Scale | Median |
|-------|------:|-------:|
| [`torture_test.sql`](examples/torture_test.sql) | 6k bulk + DML/WAL | **11.5 ms** |
| [`torture_advanced.sql`](examples/torture_advanced.sql) | geo + MathSci + datasets | **13.5 ms** |
| [`torture_unhinged.sql`](examples/torture_unhinged.sql) | 16k bulk + `VACUUM` / `REPACK` | **22.1 ms** |
| [`stress_traffic.sql`](examples/stress_traffic.sql) | 16k rows, multi-commit | **16.2 ms** |
| [`benchmark_torture.sql`](examples/benchmark_torture.sql) | joins + aggregates | **16.2 ms** |
| [`stress_bulk_load.sql`](examples/benchmarks/stress_bulk_load.sql) | 150k bulk | **16.2 ms** |
| [`stress_wal_churn.sql`](examples/benchmarks/stress_wal_churn.sql) | 35k WAL churn | **15.3 ms** |
| [`stress_analytics_mix.sql`](examples/benchmarks/stress_analytics_mix.sql) | 8k / 32k join | **12.8 ms** |

Also: [`torture_omnibus.sql`](examples/torture_omnibus.sql) (SQL + GQL + geo + MathSci + procs/triggers).

```bash
python scripts/stress_torture_histogram.py --astral build/astraldb --runs 3 --output media/stress_torture_histogram.png
```

### MathSci PINN

Physics-informed demos in SQL. Use column name **`mdl`** for the model cell (`model` is reserved).

![MathSci PINN benchmarks](media/benchmark_math_sci_pinn_plot.png)

| Harness | Grid × epochs | Median |
|---------|--------------:|-------:|
| [`benchmark_pinn_tdse_1d.sql`](examples/benchmarks/benchmark_pinn_tdse_1d.sql) | 32 × 8 | **64.9 ms** |
| [`benchmark_pinn_navier_stokes_3d.sql`](examples/benchmarks/benchmark_pinn_navier_stokes_3d.sql) | 24 × 8 | **63.5 ms** |

### MathSci inference & macro Fokker–Planck

![MCTS, Bayesian, and NFP benchmark](media/benchmark_math_sci_inference_nfp_plot.png)

| Harness | Workload | Median |
|---------|----------|-------:|
| [`benchmark_math_sci_inference_nfp.sql`](examples/benchmarks/benchmark_math_sci_inference_nfp.sql) | 32-grid NFP × 8 epochs + MCTS + Bayesian | **47.2 ms** |

## Datasets

```sql
CREATE TABLE src (id INT, a INT, b TEXT, c TEXT, d TEXT);
INSERT INTO src BULK 1000 START 1 STEP 1;
CREATE DATASET snap AS TABLE src;
INSERT INTO src BULK 10 START 1 STEP 1;
CREATE DATASET snap AS TABLE src;

CREATE TABLE dst (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET snap VERSION 1 INTO dst;
LOAD DATASET bench INTO gen;
```

See [`examples/sql_dataset.sql`](examples/sql_dataset.sql), [`examples/sql_dataset_versioning.sql`](examples/sql_dataset_versioning.sql).

## Build

**InsurgeNT** (optional):

```bash
pip install insurgent && cd AstralDB && insurgent build
```

**CMake** (matches [CI](.github/workflows/ci.yml)):

```bash
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ci
ctest --test-dir build-ci -L fast --output-on-failure   # same as CI
./build-ci/run_tests                                     # full doctest (170 cases)
```

Linux/macOS: `bash scripts/ci/build_and_test.sh` (full `ctest`).

**CI** — `ctest -L fast` (excludes perf-labelled tests), `examples/torture_test.sql` CLI smoke (`-O2 --time-sql`), Quasar `pytest`. Optional full example sweep: `ASTRALDB_RUN_EXAMPLES=1 ./build-ci/run_tests` (top-level `examples/*.sql` only; see [`docs/Examples.md`](docs/Examples.md)).

## Releases

| Tag | Notes |
|-----|--------|
| [`v1.0`](https://github.com/bumbelbee777/AstralDB/releases/tag/v1.0) | Initial public release |
| **`v2.0`** | MathSci inference, PINN, memory guards, Quasar 2.0 |

| Channel | Trigger | Workflow |
|---------|---------|----------|
| **Nightly** | Push to `main` / `master`, or manual | [`nightly.yml`](.github/workflows/nightly.yml) → tag [`nightly`](https://github.com/bumbelbee777/AstralDB/releases/tag/nightly) |
| **Release** | Push `v*` tag | [`release.yml`](.github/workflows/release.yml) |

Both publish flat platform binaries, the Quasar wheel, and **`SHA256SUMS`**:

| Asset | Platform |
|-------|----------|
| `astraldb-linux-amd64` | Linux x86_64 |
| `astraldb-macos-arm64` | macOS Apple Silicon |
| `astraldb-windows-amd64.exe` | Windows x86_64 |
| `quasar-*-py3-none-any.whl` | Quasar (Python ≥3.10) |

```bash
sha256sum -c SHA256SUMS
chmod +x astraldb-linux-amd64   # Unix
pip install quasar-*-py3-none-any.whl
export QUASAR_ASTRALDB=/path/to/astraldb-linux-amd64
```

CI workflow artifacts include per-file `*.sha256` sidecars for the same binaries and wheel.

## Documentation

| Topic | Doc |
|-------|-----|
| Overview & caveats | [`docs/Overview.md`](docs/Overview.md) |
| Examples index | [`docs/Examples.md`](docs/Examples.md) |
| Quasar orchestration | [`docs/Quasar.md`](docs/Quasar.md) |
| Geospatial | [`docs/Geospatial.md`](docs/Geospatial.md) |
| MathSci | [`docs/MathSciPinn.md`](docs/MathSciPinn.md), [`docs/MathSciInference.md`](docs/MathSciInference.md), [`docs/MathSciFokkerPlanck.md`](docs/MathSciFokkerPlanck.md) |
| Release checklist | [`docs/RELEASING.md`](docs/RELEASING.md) |
