# Example SQL scripts

Runnable scripts under [`examples/`](../examples/) are the living contract alongside [`tests/`](../tests/) (**63** `.sql` files: **55** in `examples/`, **8** in `examples/benchmarks/`). Run a script with:

```text
astraldb -O2 -m -s examples/<script>.sql
```

Use `-O4` and `--time-sql` for benchmarks. Perf-tier harnesses (`torture_*`, `nuke.sql`, `benchmark_*`, `stress_*`) are labeled **`perf`** in CTest; see [`Usage.md`](Usage.md) for CLI flags.

## SQL dialect and analytics

| Script | Topic |
|--------|--------|
| `hello.sql`, `basic_operations.sql` | Minimal DDL/DML |
| `sql92_03_coverage.sql` | SQL-92/99/2003 surface |
| `sql92_analytics.sql` | Aggregates, `COALESCE`, windows |
| `sql99_recursive.sql` | `WITH RECURSIVE` |
| `sql_window_frames.sql` | Window frames |
| `sql_olap.sql`, `sql_olap_grouping.sql` | ROLLUP/CUBE/GROUPING SETS |
| `sql_merge_upsert.sql`, `sql_merge_exprs.sql` | MERGE / upsert |
| `sql_dialect_compat.sql` | SQLite/Postgres/Oracle/DuckDB sugar |
| `sql_standard_features.sql` | Temporal, JSON/XML, MATCH |
| `sql_sequence_identity.sql` | Sequences, identity columns |
| `sql_indexes.sql`, `sql_text_search.sql` | Indexes, FTS |
| `sql_xml.sql` | XML scalars |
| `sql_timeseries.sql` | `TIME_BUCKET`, compression codecs |
| `sql_variant_vacuum.sql` | `VARIANT`, `VACUUM` / maintenance |
| `sql_compliance.sql` | Compliance-oriented checks |
| `edge_cases.sql`, `performance_test.sql`, `join_bench.sql` | Edge cases and ad hoc perf (not default CI) |

## Graph (GQL)

| Script | Topic |
|--------|--------|
| `graph_analytics.sql` | Paths, shortest path, PageRank |
| `graph_cypher.sql` | Cypher-style patterns |
| `graph_social.sql`, `graph_finance.sql` | Domain graphs |

## Geospatial

| Script | Topic |
|--------|--------|
| `sql_geospatial.sql` | 2D points |
| `sql_geospatial_polygon.sql` | 2D polygons |
| `sql_geospatial_mesh.sql` | 3D meshes, glTF |
| `sql_geospatial_terrain.sql` | DEM / slope |

See [`Geospatial.md`](Geospatial.md).

## MathSci, ML, PINN

| Script | Topic |
|--------|--------|
| `sql_math_sci.sql` | Math, signal, ODE/PDE, NLP, embeddings |
| `math_sci_lm_tiny.sql` | Tiny LM train + inference on a short story |
| `math_sci_pinn_tdse_1d.sql` | 1-D TDSE PINN demo |
| `math_sci_pinn_navier_stokes_3d.sql` | 3-D Navier–Stokes PINN demo |
| `math_sci_inference_nfp.sql` | MCTS, Bayesian inference, neural macro Fokker–Planck |

See [`MathSciLmTrain.md`](MathSciLmTrain.md), [`MathSciPinn.md`](MathSciPinn.md), [`MathSciInference.md`](MathSciInference.md), [`MathSciFokkerPlanck.md`](MathSciFokkerPlanck.md), and sibling `MathSci*.md` docs.

## Procedures, triggers, security

| Script | Topic |
|--------|--------|
| `sql_procedure.sql`, `sql_procedure_plpgsql.sql`, `sql_procedure_plsql.sql` | Stored procedures |
| `sql_trigger.sql` | Triggers |
| `security_rbac.sql`, `security_users.sql` | RBAC, users |
| `security_memory.sql` | Spike-aware memory guards |

See [`StoredProcedures.md`](StoredProcedures.md), [`Triggers.md`](Triggers.md).

## Storage, datasets, hybrid

| Script | Topic |
|--------|--------|
| `sql_dataset.sql`, `sql_dataset_versioning.sql` | Named datasets |
| `sql_hybrid_storage.sql` | Hybrid row/columnar |
| `sql_advanced_types.sql`, `sql_advanced_subset.sql` | STRUCT/MAP/VECTOR/… |
| `data_exchange.sql` | Import/export patterns |

## Torture and benchmarks

| Script | Tier | Topic |
|--------|------|--------|
| `torture_test.sql` | perf (CI smoke via `astraldb -O2 --time-sql`) | Bulk DML, WAL, predicates |
| `torture_advanced.sql` | perf | Geo, MathSci, datasets |
| `torture_unhinged.sql` | perf | Large bulk + maintenance |
| `torture_omnibus.sql` | perf | SQL + MathSci + GQL + 2D/3D geo + joins at scale |
| `benchmark_torture.sql`, `nuke.sql` | perf | Join/analytics stress |
| `benchmarks/*` | perf / plot | Cross-engine analytic baseline, bulk/WAL suites |

Subdirectory `examples/benchmarks/` holds harnesses used by plotting scripts (`benchmark_torture_plot.py`, `stress_torture_histogram.py`, PINN/NFP generators).

### CI and doctest coverage

| Layer | What runs |
|-------|-----------|
| **GitHub Actions** | `ctest -L fast` (unit tests; excludes perf-labelled cases), `astraldb -O2 --time-sql examples/torture_test.sql`, Quasar `pytest` |
| **Full doctest** | `./build-ci/run_tests` — **170** cases, **1171** assertions (includes perf tier with `--test-suite=perf`) |
| **Example sweep** | `ASTRALDB_RUN_EXAMPLES=1 run_tests` runs the `examples: every *.sql` case: **top-level** `examples/*.sql` only (skips `benchmarks/` and perf harnesses; four slow scripts skipped unless `ASTRALDB_RUN_ALL_EXAMPLES=1`) |

Perf harnesses and `examples/benchmarks/*` are for local benches and README charts, not the default CI matrix.
