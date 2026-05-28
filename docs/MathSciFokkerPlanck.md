# MathSci: neural macro Fokker–Planck

**NFP** = forward Kolmogorov step for a **wealth distribution** on a 1-D grid with quant-macro features that go beyond textbook linear Fokker–Planck:

- **Borrowing barrier** (reflecting / absorbing mass at `a_min`)
- **Mean-field capital feedback** (aggregate `K` vs clearing `K*`)
- **State-dependent diffusion** (idiosyncratic risk per grid point)
- **Neural drift correction** `L[n]` (MLP policy residual from `PREDICT` / training loop)

This is the PDE behind heterogeneous-agent (HANK) distribution dynamics; the SQL surface is one fused step or multi-step march for benchmarks.

## Builtins

| Builtin | Args | Returns |
|---------|------|---------|
| `NFP_MACRO_STEP(g, grid, drift, diffusion, neural_corr, agg_K, K_star, barrier_a, dt)` | Density and grid `L[n]`, same-length coeffs | Updated density `L[n]` |
| `NFP_MACRO_MARCH(…, steps)` | Same + integer steps | Density after `steps` explicit steps |
| `NFP_MACRO_MOMENTS(g, grid)` | Density + grid | `L[4]: mass, mean_wealth, variance, gini_proxy` |

Conservative finite-volume explicit step with CFL clamp; mass renormalized each step.

## Workflow with PINNs / MLP

1. `PREDICT(mdl, …)` → savings drift per collocation point.
2. Map to grid → `neural_corr` list.
3. `NFP_MACRO_MARCH` advances `g`; read `NFP_MACRO_MOMENTS` for market clearing residual.

See [`MathSciPinn.md`](MathSciPinn.md), [`MathSciModel.md`](MathSciModel.md).

## Example & benchmark

- Demo: [`examples/math_sci_inference_nfp.sql`](../examples/math_sci_inference_nfp.sql)
- Harness: [`examples/benchmarks/benchmark_math_sci_inference_nfp.sql`](../examples/benchmarks/benchmark_math_sci_inference_nfp.sql)
