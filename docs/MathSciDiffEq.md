# MathSci differential equations (DiffEq / DEQ)

Ergonomic, **fused** ODE integration for training and inference. One `DEQ_INTEGRATE` call replaces hundreds of SQL round-trips through `ODE_MARCH`, keeping wall time in the **tens of milliseconds** for typical PINN state sizes.

Implementations: `sources/Database/MathSciSolves.*` (integrators) and existing per-step builtins (`ODE_*`, `SOLVE_ODE`).

## Quick start (Diffax-style)

```sql
-- Time grid 0 .. 1 with 101 points
SELECT DEQ_LINSPACE(0, 1, 101) AS ts;

-- March state with fused RK4 (constant slopes per step — refresh k each SQL epoch)
SELECT DEQ_INTEGRATE(
  'RK4',
  'L[2]:1,0',
  '0.01',
  '100',
  'L[2]:0.1,0.2',
  'L[2]:0.1,0.2',
  'L[2]:0.1,0.2',
  'L[2]:0.1,0.2'
) AS y_end;

-- Adaptive step (RK4 step-doubling error estimate)
SELECT DEQ_ADAPT(
  'RK4',
  'L[2]:1,0',
  '0',
  '1',
  '0.05',
  '1e-4',
  '1e-6',
  'L[2]:0.1,0.2',
  'L[2]:0.1,0.2',
  'L[2]:0.1,0.2',
  'L[2]:0.1,0.2'
) AS y_at_t1;
```

## Functions

| Function | Arity | Description |
|----------|-------|-------------|
| `DEQ_INTEGRATE(method, y, dt, steps, k1, k2, k3, k4)` | 8 | Fused explicit march (float path, ≤8192 steps). |
| `DEQ_ADAPT(method, y, t0, t1, h0, rtol, atol, k1..k4)` | 11 | Adaptive RK4 pair; adjusts step toward `t1`. |
| `DEQ_LINSPACE(t0, t1, n)` | 3 | Uniform time grid `L[n]:…`. |

Methods: `EULER`, `HEUN`, `RK2`, `MIDPOINT`, `RK3`, `RK4`, `DOPRI5`/`TSIT5` (fixed-step RK4 kernel), plus aliases matching `SOLVE_ODE`.

## Performance notes

- **`ODE_MARCH`** now delegates to the same fused integrator as `DEQ_INTEGRATE`.
- **`PREDICT`** caches compiled `MB1:` models in-process (LRU 64 entries) and reuses activation buffers.
- Cap: `MaxOdeIntegrateSteps = 8192`, state length `MaxSolveLen`.

Benchmark: `examples/benchmarks/benchmark_math_sci_perf.sql` + `scripts/bench_math_sci_perf.py`.

## Related

- [`MathSciSolves.md`](MathSciSolves.md) — per-step ODE/SDE/PDE builtins
- [`MathSciModel.md`](MathSciModel.md) — `PREDICT` / binary models
- [`MathSciPinn.md`](MathSciPinn.md) — PINN workflows
