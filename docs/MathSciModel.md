# MathSci models: binary I/O and PREDICT

Feed-forward MLP weights for PINN and classifier demos: build, serialize to a compact **binary** cell, import/load, fingerprint, and run inference with **`PREDICT()`** inside SQL.

Implementations: `sources/Database/MathSciModel.*`.

## Wire formats

| Prefix | Meaning |
|--------|---------|
| `MB1:` + hex | Version-1 binary blob (canonical after import/serialize) |
| `M[act,...]:` + `T[r,c]:…` | Human-readable text (semicolon-separated layers in build API) |
| `LW[n]:` + `T…;T…` | **Build** weight list (semicolons avoid commas inside `T[r,c]` cells) |

## Functions

| Function | Arity | Description |
|----------|-------|-------------|
| `MATHSCI_MODEL_BUILD(activations, weights)` | 2 | `L[k]:sigmoid,linear` + `LW[k]:T…;T…` → `MB1:…` |
| `MATHSCI_MODEL_SERIALIZE(model)` | 1 | Canonical binary cell |
| `MATHSCI_MODEL_IMPORT(model)` | 1 | Parse and validate → `MB1:…` |
| `MATHSCI_MODEL_LOAD(model)` | 1 | Alias of import |
| `MATHSCI_MODEL_FINGERPRINT(model)` | 1 | Stable hex hash |
| `PREDICT(model, features)` | 2 | Forward pass → `L[n]:…` output |

Activations per layer: `linear`, `sigmoid`, `tanh`, `relu`. Default: `sigmoid` on hidden layers, `linear` on the last layer when omitted.

## Example

```sql
SELECT MATHSCI_MODEL_BUILD(
  'L[2]:sigmoid,linear',
  'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.3,0.1,-0.2,0.4, 0.2,-0.1,0.3,0.2'
) AS blob;

SELECT PREDICT(blob, 'L[2]:0.5,0.2') AS y
FROM (SELECT blob FROM trained_model LIMIT 1) t;
```

Store `MB1:…` cells in `TEXT` columns; round-trip with `MATHSCI_MODEL_IMPORT(MATHSCI_MODEL_SERIALIZE(model))`.

## Benchmarks

| Script | Plot |
|--------|------|
| [`examples/benchmarks/benchmark_pinn_tdse_1d.sql`](../examples/benchmarks/benchmark_pinn_tdse_1d.sql) | [`scripts/plot_math_sci_pinn_bench.py`](../scripts/plot_math_sci_pinn_bench.py) |
| [`examples/benchmarks/benchmark_pinn_navier_stokes_3d.sql`](../examples/benchmarks/benchmark_pinn_navier_stokes_3d.sql) | same |

## Related

- [`MathSciPinn.md`](MathSciPinn.md), [`MathSciAutograd.md`](MathSciAutograd.md), [`MathSciClassify.md`](MathSciClassify.md)
