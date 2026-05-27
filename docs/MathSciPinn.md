# Physics-informed neural networks (PINN) in SQL

PINNs minimize a **PDE residual** at collocation points plus **boundary / initial** penalties. AstralDB has no dedicated `PINN_*` opcode; demos compose **finite-difference derivatives** of a small MLP ( `MATVEC`, `SIGMOID` / `LOGISTIC`, `VECTOR` cells) with **`MSE_LOSS`**, **`AD_CHAIN`**, and—for complex wavefunctions—**Wirtinger** builtins from [`MathSciAutograd.md`](MathSciAutograd.md).

## Pattern

1. **Collocation table** — rows `(x)` or `(x, y, z)` or `(x, t)` with optional boundary tags.
2. **Weights table** — `W1`, `W2` as `T[r,c]:…` / `L[n]:…` cells for a 1–2 hidden-layer net.
3. **Forward** — `h = SIGMOID(MATVEC(W1, inputs))`, `out = MATVEC(W2, h)` (split across CTE columns).
4. **Spatial / temporal derivatives** — evaluate the network at `x ± ε` (and `y`, `z`, `t` as needed); finite-difference quotients build `∂u/∂x`, `∇²u`, `∂ψ/∂t`, etc.
5. **Residual** — form the PDE left-hand side minus right-hand side as lists; `MSE_LOSS(residual, zeros)` for collocation loss.
6. **Training** — unroll a few gradient-descent steps with `AD_GRAD_SIGMOID`, `AD_CHAIN`, `VECTOR_ADD` (toy step size).

Parser note: do not nest scalar builtins in one argument; use CTEs.

## Demos

| Script | Model |
|--------|--------|
| [`examples/math_sci_pinn_tdse_1d.sql`](../examples/math_sci_pinn_tdse_1d.sql) | 1-D time-dependent Schrödinger (complex `CV[]` state, Wirtinger-friendly residuals) |
| [`examples/math_sci_pinn_navier_stokes_3d.sql`](../examples/math_sci_pinn_navier_stokes_3d.sql) | Steady 3-D incompressible Navier–Stokes–style residual (velocity + pressure; finite-difference Laplacian and divergence) |

These scripts use **small** collocation sets and **few** explicit update steps to stay within CI-friendly runtime; scale points and epochs for research runs with `astraldb -O4 --time-sql … -m`.

## Related

- [`MathSciSolves.md`](MathSciSolves.md) — classical PDE steppers (`PDE_HEAT_*`, `PDE_POISSON_*`, …)
- [`MathSciAutograd.md`](MathSciAutograd.md), [`MathSciSignal.md`](MathSciSignal.md)
- [`examples/sql_math_sci.sql`](../examples/sql_math_sci.sql)
