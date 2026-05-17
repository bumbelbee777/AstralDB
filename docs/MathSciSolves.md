# MathSci differential equation solvers

Explicit steppers for common **ODE**, **SDE**, and **1-D PDE** patterns. Implementations live in `sources/Database/MathSciSolves.*` and are exposed as SQL scalar builtins.

## ODE

| Function | Arity | Description |
|----------|-------|-------------|
| `ODE_EULER(y, slope, dt)` | 3 | Explicit Euler: `y + dt·slope`. `y` and `slope` are equal-length lists; `dt` is numeric. |
| `ODE_RK4(y, k1, k2, k3, k4, dt)` | 6 | Classical RK4 step using caller-supplied stage slopes (same length as `y`). |
| `ODE_HEUN(y, k1, k2, dt)` | 4 | Heun / RK2: `y + (dt/2)(k1 + k2)`. |
| `ODE_MIDPOINT(y, k_mid, dt)` | 3 | Explicit midpoint step. |
| `ODE_IMPLICIT_EULER(y, lambda, dt)` | 3 | Diagonal implicit Euler for `y' = λ⊙y`: `y / (1 − dt·λ)` per component. |
| `SOLVE_ODE(method, y, a, b, c, d, dt)` | 7 | Configurable dispatcher: `method` is `EULER`, `HEUN`, `MIDPOINT`, `RK4`, or `IMPLICIT` (unused stage args may be `L[1]:0`). |

## SDE

| Function | Arity | Description |
|----------|-------|-------------|
| `SDE_EULER(y, drift, diffusion, z, dt)` | 5 | Euler–Maruyama: `y + drift·dt + diffusion·√dt·z`. |
| `SDE_GBM(y, mu, sigma, dt, z)` | 5 | One-step geometric Brownian motion (scalar). |
| `SDE_OU(x, mu, theta, sigma, dt, z)` | 6 | Ornstein–Uhlenbeck exact transition (scalar). |
| `SDE_MILSTEIN(y, mu, sigma, dt, z)` | 5 | Scalar Milstein step for GBM-type SDE (strong order 1.0 correction). |

`z` is a standard normal shock (use `RANDOM_NORMAL` or fixed seeds via `SETSEED`).

## PDE (1-D, uniform grid)

Grid spacing is normalized (`dx = 1` in the discrete operators). Boundary values are held fixed (Dirichlet-style) at index `0` and `N-1`.

| Function | Arity | Description |
|----------|-------|-------------|
| `PDE_HEAT_STEP(u, alpha, dt, dx)` | 4 | Heat equation explicit FTCS step (stability requires `alpha·dt/dx² ≤ 0.5`). |
| `PDE_POISSON_STEP(u, f, omega)` | 3 | Poisson \(-u'' = f\): one Jacobi relaxation with weight `omega`. |
| `PDE_ADVECTION_STEP(u, c, dt, dx)` | 4 | 1-D upwind advection step. |
| `PDE_WAVE_STEP(u_prev, u_curr, c, dt, dx)` | 5 | 1-D wave equation leapfrog step. |

Chain multiple statements (or CTEs) to march in time or iterate to convergence.

## Usage pattern

```sql
-- One OU step on a scalar state
SELECT SDE_OU(x, 0.0, 2.0, 0.3, 0.01, RANDOM_NORMAL()) AS x_next FROM state;

-- Vector EM step
SELECT SDE_EULER(y, drift, vol, noise, '0.01') AS y_next FROM batch;
```

## Limits

Maximum list length: `1_048_576` (`MaxSolveLen`).

## Related

- `docs/MathSciSignal.md`, `docs/MathSciAutograd.md`
- `examples/sql_math_sci.sql`
