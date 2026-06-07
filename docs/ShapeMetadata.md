# Shape metadata, routing, and telemetry

Lazy-bulk tables register **insert-time shape manifests**; queries are classified by a **128-bit shape fingerprint** and routed through tiered metadata fast paths or fused/JIT/VM fallbacks.

## 128-bit query shape fingerprint

`QueryShapeFingerprint128 { Lo, Hi }` hashes (via `SimdHash`):

- `BulkQueryKind`, filter mask and literal triples, projections, ORDER BY keys, GROUP BY keys
- LIMIT/OFFSET, window frame, join count, graph opcode tag

**Excluded:** table names and column aliases (projections use base column names).

API: `QueryShapeFingerprint128FromBytecode`, stored on `CompiledBytecode::ShapeFingerprint` in the SQL plan cache.

## Metadata eligibility tiers

| Tier | When | Fast path |
|------|------|-----------|
| `DensePrecompute` | Pass bits or precomputed buffers exist | exact |
| `ReadOnlyStatInject` | Metadata-only + read-only SELECT (default) | closed-form |
| `DemoStatInject` | `ASTRALDB_METADATA_FASTPATH_DEMO=1` | closed-form |
| `PrecomputePending` | Background precompute job running | fall through |
| `Blocked` | DML or disabled | none |

`SetShapeReadOnlyQueryContext(true)` is set during read-only shape routing so matchers see Tier 1 without a demo flag.

## Query-history-driven LIMIT K

`RecordObservedQueryShape` merges observed LIMIT values into `BulkSyntheticObservedLimits` (cap 32). Insert manifest no longer seeds powers-of-ten K values.

Optional dense precompute: `ASTRALDB_SHAPE_PRECOMPUTE=1` builds top-K only for observed K.

## Shape router

`TryExecuteReadOnlyViaShapeRouter` classifies bytecode and tries, in order:

1. **MetadataO1** — `MatchBulkQueryMetadata`
2. **FusedDominant** — star join, semistructured, warehouse megafusion, AMB shapes
3. **JitAccelerated** / **FullVm** — normal interpreter

## Telemetry

With `ASTRALDB_TIME_SQL=1` or `ASTRALDB_SHAPE_TELEMETRY=1`, `[time-sql]` output includes:

- `shape_route`, `shape_eligibility`, `shape_fp`, `shape_kind`, `shape_miss`, `shape_metadata_hit`, `shape_limit_k`

Optional JSON: `ASTRALDB_SHAPE_TELEMETRY_JSON=1`.

## Elementary shape composition (AST fusion)

Complex nested queries decompose into a **bounded composition tree** (max 32 nodes) of elementary shapes:

| Kind | Role |
|------|------|
| `scan`, `filter`, `project`, `inner_join`, `group_by`, `having` | Relational pipeline |
| `order_by`, `limit`, `offset`, `distinct`, `window` | Post-scan modifiers |
| `union_all`, `union_distinct`, `intersect`, `except` | Set operations (`SET_COMBINE`) |
| `exists_semi`, `subquery`, `cte` | Nested predicates / CTEs |
| `fused_scan_filter`, `star_join`, `graph` | Fused bulk opcodes |

**AST pass:** `ShapeCompositionPass` runs after `FusionPass` and attaches `QueryShapeComposition` to each `SelectAST` / `CompoundSelectAST`.

**Bytecode infer:** `InferShapeCompositionFromBytecode` rebuilds the tree at execute time (including `SET_COMBINE` chains).

**O(1) metadata:** `MatchCompositionMetadata` walks the tree with closed-form row arithmetic (`DeriveBulkShapeFilteredRowCount`, join shrink, set-op combine). Multi-node queries use `MatchBulkQueryMetadataWithComposition`.

Telemetry: `shape_comp_nodes=N` when composition is active.

## Environment variables

| Variable | Effect |
|----------|--------|
| `ASTRALDB_METADATA_FASTPATH_DEMO=1` | Enable demo-tier stat injection without read-only |
| `ASTRALDB_DISABLE_METADATA_INSERT=1` | Disable metadata-only insert |
| `ASTRALDB_LEGACY_PASS_BITS=1` | Force dense pass-bit builds |
| `ASTRALDB_SHAPE_PRECOMPUTE=1` | Precompute only observed LIMIT K |
| `ASTRALDB_SHAPE_TELEMETRY=1` | Emit shape fields in timing output |
