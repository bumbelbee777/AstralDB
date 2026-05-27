# MathSci differential equation solvers

Explicit steppers, **iterative linear / root / PDE solvers**, and time marching for common **ODE**, **SDE**, and **1-D PDE** patterns. Implementations live in `sources/Database/MathSciSolves.*` and are exposed as SQL scalar builtins.

## ODE

| Function | Arity | Description |
|----------|-------|-------------|
| `ODE_EULER(y, slope, dt)` | 3 | Explicit Euler: `y + dt·slope`. `y` and `slope` are equal-length lists; `dt` is numeric. |
| `ODE_RK4(y, k1, k2, k3, k4, dt)` | 6 | Classical RK4 step using caller-supplied stage slopes (same length as `y`). |
| `ODE_RK3(y, k1, k2, k3, dt)` | 5 | Classical RK3 (SSP) step: `y + (dt/6)(k1 + 4·k2 + k3)`. |
| `ODE_HEUN(y, k1, k2, dt)` | 4 | Heun / RK2: `y + (dt/2)(k1 + k2)`. |
| `ODE_MIDPOINT(y, k_mid, dt)` | 3 | Explicit midpoint step. |
| `ODE_IMPLICIT_EULER(y, lambda, dt)` | 3 | Diagonal implicit Euler for `y' = λ⊙y`: `y / (1 − dt·λ)` per component. |
| `ODE_TRAPEZOID(y, k0, k1, dt)` | 4 | Explicit trapezoid: `y + (dt/2)(k0 + k1)`. |
| `ODE_SEMI_IMPLICIT(y, k1, k2, dt)` | 4 | Staged semi-implicit: `y* = y + dt·k1`, then `y* + dt·k2`. |
| `ODE_CRANK_NICOLSON(y, lambda, k, dt)` | 4 | Diagonal Crank–Nicolson: `(y + (dt/2)·k) / (1 − (dt/2)·λ)`. |
| `ODE_ADAMS_BASHFORTH2(y, k_curr, k_prev, dt)` | 4 | Second-order Adams–Bashforth with supplied slopes. |
| `ODE_MARCH(method, y, a, b, c, d, dt, steps)` | 8 | Repeat `SOLVE_ODE` step `steps` times (constant stage vectors). |
| `SOLVE_ODE(method, y, a, b, c, d, dt)` | 7 | Configurable dispatcher: `EULER`, `HEUN`, `MIDPOINT`, `RK3`, `RK4`, `IMPLICIT`, `TRAPEZOID`, `SEMI_IMPLICIT`, `CRANK_NICOLSON`, `AB2` (aliases `SEMI`, `CN`; unused stage args may be `L[1]:0`). |

## SDE

| Function | Arity | Description |
|----------|-------|-------------|
| `SDE_EULER(y, drift, diffusion, z, dt)` | 5 | Euler–Maruyama: `y + drift·dt + diffusion·√dt·z`. |
| `SDE_GBM(y, mu, sigma, dt, z)` | 5 | One-step geometric Brownian motion (scalar). |
| `SDE_OU(x, mu, theta, sigma, dt, z)` | 6 | Ornstein–Uhlenbeck exact transition (scalar). |
| `SDE_MILSTEIN(y, mu, sigma, dt, z)` | 5 | Scalar Milstein step for GBM-type SDE (strong order 1.0 correction). |
| `SDE_MARCH(y, drift, diffusion, z, dt, steps)` | 6 | Repeat Euler–Maruyama `steps` times with the same shocks. |

`z` is a standard normal shock (use `RANDOM_NORMAL` or fixed seeds via `SETSEED`).

## PDE (1-D, uniform grid)

Grid spacing is normalized (`dx = 1` in the discrete operators). Boundary values are held fixed (Dirichlet-style) at index `0` and `N-1`.

| Function | Arity | Description |
|----------|-------|-------------|
| `PDE_HEAT_STEP(u, alpha, dt, dx)` | 4 | Heat equation explicit FTCS step (stability requires `alpha·dt/dx² ≤ 0.5`). |
| `PDE_HEAT_MARCH(u, alpha, dt, dx, steps)` | 5 | Repeat `PDE_HEAT_STEP` `steps` times. |
| `PDE_POISSON_STEP(u, f, omega)` | 3 | Poisson \(-u'' = f\): one Jacobi relaxation with weight `omega`. |
| `PDE_POISSON_GS_STEP(u, f)` | 2 | One Gauss–Seidel relaxation for Poisson. |
| `PDE_POISSON_SOR_STEP(u, f, omega)` | 3 | One SOR relaxation for Poisson. |
| `PDE_POISSON_SOLVE(u, f, omega, max_iters, tol)` | 5 | Iterate Jacobi until max change `< tol` or `max_iters`. |
| `PDE_ADVECTION_STEP(u, c, dt, dx)` | 4 | 1-D upwind advection step. |
| `PDE_WAVE_STEP(u_prev, u_curr, c, dt, dx)` | 5 | 1-D wave equation leapfrog step. |
| `SOLVE_PDE(method, u, f, omega, max_iters, tol, alpha, dt, dx)` | 9 | Dispatcher: `POISSON`, `POISSON_GS`, `POISSON_SOR`, `HEAT_MARCH` (uses `max_iters` as step count for heat). |

Chain multiple statements (or CTEs) to march in time or iterate to convergence.

## Iterative linear solvers (dense Ax = b)

Matrix `A` is a square `T[n,n]:…` cell; vectors are `L[n]:…`.

| Function | Arity | Description |
|----------|-------|-------------|
| `LINEAR_JACOBI_STEP(A, x, b)` | 3 | One Jacobi iteration. |
| `LINEAR_GS_STEP(A, x, b)` | 3 | One Gauss–Seidel iteration. |
| `LINEAR_SOR_STEP(A, x, b, omega)` | 4 | One SOR iteration. |
| `LINEAR_RICHARDSON_STEP(A, x, b, alpha)` | 4 | Richardson: `x + α(b − Ax)`. |
| `LINEAR_CG_SOLVE(A, x0, b, max_iters, tol)` | 5 | Conjugate gradient for SPD systems. |
| `SOLVE_LINEAR(method, A, x, b, max_iters, tol, param)` | 7 | Iterate until residual ∞-norm `< tol`: `JACOBI`, `GS`, `SOR`, `RICHARDSON`, `CG` (`param` = ω or α). |

## Root finding (scalar)

| Function | Arity | Description |
|----------|-------|-------------|
| `ROOT_NEWTON_STEP(x, f(x), f′(x))` | 3 | One Newton step. |
| `ROOT_SECANT_STEP(x0, x1, f0, f1)` | 4 | One secant step. |
| `ROOT_BISECT_STEP(lo, hi, f(lo), f(hi))` | 4 | Bisection midpoint (requires opposite signs). |
| `ROOT_HALLEY_STEP(x, f, f′, f″)` | 4 | One Halley step. |
| `SOLVE_ROOT(method, a, b, c, d)` | 5 | One step dispatcher: `NEWTON`, `SECANT`, `BISECT`, `HALLEY`. |

## Usage pattern

```sql
-- One OU step on a scalar state
SELECT SDE_OU(x, 0.0, 2.0, 0.3, 0.01, RANDOM_NORMAL()) AS x_next FROM state;

-- Vector EM step
SELECT SDE_EULER(y, drift, vol, noise, '0.01') AS y_next FROM batch;

-- CG solve 2×2 SPD system
SELECT LINEAR_CG_SOLVE('T[2,2]:4,1,1,3', 'L[2]:0,0', 'L[2]:1,2', 64, 1e-8) AS x;

-- March explicit Euler 10 steps with fixed slope
SELECT ODE_MARCH('EULER', 'L[1]:1', 'L[1]:0.5', 'L[1]:0', 'L[1]:0', 'L[1]:0', '0.01', 10) AS y_end;
```

## Limits

Maximum list length: `1_048_576` (`MaxSolveLen`).

## Related

- [`MathSciSignal.md`](MathSciSignal.md), [`MathSciAutograd.md`](MathSciAutograd.md), [`MathSciPinn.md`](MathSciPinn.md) (physics-informed neural networks in SQL)
- [`examples/sql_math_sci.sql`](../examples/sql_math_sci.sql), [`examples/math_sci_pinn_tdse_1d.sql`](../examples/math_sci_pinn_tdse_1d.sql), [`examples/math_sci_pinn_navier_stokes_3d.sql`](../examples/math_sci_pinn_navier_stokes_3d.sql)
