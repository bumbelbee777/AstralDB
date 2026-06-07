# Example SQL scripts



Runnable scripts under [`examples/`](../examples/) are the living contract alongside [`tests/`](../tests/) (**62** top-level `.sql` files + **33** harnesses under `examples/benchmarks/`). Run a script with:



```text

astraldb -O2 -m -s examples/<script>.sql

```



Use `-O4` and `--time-sql` for benchmarks. Perf-tier harnesses (`torture_*`, `nuke.sql`, `benchmark_*`, `stress_*`, warehouse suites) are labeled **`perf`** in CTest; see [`Usage.md`](Usage.md) for CLI flags.



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



See [`MathSciCore.md`](MathSciCore.md), [`MathSciMl.md`](MathSciMl.md), [`MathSciInference.md`](MathSciInference.md).



## Procedures, triggers, security



| Script | Topic |

|--------|--------|

| `sql_procedure.sql`, `sql_procedure_plpgsql.sql`, `sql_procedure_plsql.sql`, `sql_procedure_tsql.sql` | Stored procedures |

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

| `nuke_materialize.sql`, `nuke_materialize_setup.sql`, `nuke_materialize_query.sql` | Materialized bulk + query path |



Column shard on-disk layout: [`ColumnShardFormat.md`](ColumnShardFormat.md).



## Torture and benchmarks



### Top-level perf harnesses



| Script | Tier | Topic |

|--------|------|--------|

| `torture_test.sql` | perf (CI smoke via `astraldb -O2 --time-sql`) | Bulk DML, WAL, predicates |

| `torture_advanced.sql` | perf | Geo, MathSci, datasets |

| `torture_unhinged.sql` | perf | Large bulk + maintenance |

| `torture_omnibus.sql` | perf | SQL + MathSci + GQL + 2D/3D geo + joins at scale |

| `benchmark_torture.sql`, `nuke.sql` | perf | Join/analytics stress |

| `nuke_profile.sql`, `nuke_200k_setup.sql` | perf | Nuke variants and profiling |



### `examples/benchmarks/` harnesses



| Harness | Plot / runner script |

|---------|---------------------|

| `benchmark_torture_unified.sql` | `scripts/benchmark_torture_plot.py` → `media/astraldb_bench.png` |

| `antimatterbomb_setup.sql`, `antimatterbomb_query.sql`, `antimatterbomb_cube.sql` | `scripts/benchmark_antimatterbomb_plot.py` |

| `neutroniumbomb_setup.sql`, `neutroniumbomb_query.sql`, `neutroniumbomb_htap.sql`, `neutroniumbomb_q*.sql` | `scripts/benchmark_neutroniumbomb_*.py`, `scripts/run_neutroniumbomb_*.py` |

| `stress_bulk_load.sql`, `stress_wal_churn.sql`, `stress_analytics_mix.sql` | `scripts/stress_torture_histogram.py` |

| `benchmark_pinn_tdse_1d.sql`, `benchmark_pinn_navier_stokes_3d.sql` | `scripts/plot_math_sci_pinn_bench.py` |

| `benchmark_math_sci_inference_nfp.sql` | `scripts/plot_math_sci_inference_nfp_bench.py` |

| `benchmark_math_sci_perf.sql` | `scripts/bench_math_sci_perf.py` |

| `_bench_*.sql`, `_bench_setup_small.sql` | `scripts/run_storage_benches.ps1` (storage micro-benches) |



Top-level orchestrators [`antimatterbomb.sql`](../examples/antimatterbomb.sql) and [`neutroniumbomb.sql`](../examples/neutroniumbomb.sql) compose the warehouse harness SQL under `examples/benchmarks/`.



### CI and doctest coverage



| Layer | What runs |

|-------|-----------|

| **GitHub Actions** | `ctest -L fast` (unit tests; excludes perf-labelled cases), `astraldb -O2 --time-sql examples/torture_test.sql`, Quasar `pytest` |

| **Full doctest** | `./build-ci/run_tests` — **222** cases, **1252** assertions (includes perf tier with `--test-suite=perf`) |

| **Example sweep** | `ASTRALDB_RUN_EXAMPLES=1 run_tests` runs the `examples: every *.sql` case: **top-level** `examples/*.sql` only (skips `benchmarks/` and perf harnesses; four slow scripts skipped unless `ASTRALDB_RUN_ALL_EXAMPLES=1`) |



Perf harnesses and `examples/benchmarks/*` are for local benches and README charts, not the default CI matrix.


