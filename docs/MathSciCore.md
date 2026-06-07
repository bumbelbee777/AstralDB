# MathSci core: signal, autograd, and solvers

SIMD-accelerated **signal processing**, **automatic differentiation** (real and complex Wirtinger), **computation graphs**, and **differential-equation solvers** exposed as SQL scalar builtins. Implementations live under `sources/Database/MathSci/` (`MathSciSignal.*`, `MathSciAutograd.*`, `MathSciAutogradGraph.*`, `MathSciSolves.*`).

Runnable contract: [`examples/sql_math_sci.sql`](../examples/sql_math_sci.sql). For models, NLP, PINN, and inference builtins see [`MathSciMl.md`](MathSciMl.md) and [`MathSciInference.md`](MathSciInference.md).

**Parser note:** scalar builtins cannot nest inside one another in a single projection argument. Compose with CTEs, multiple columns, or separate statements.

---

## Signal processing and first-order autograd

Sequence arguments accept `LIST` wire cells (`L[n]:…`) or `VECTOR` payloads (`V[n]:…`). Results are `LIST` cells unless noted.

| Function | Arity | Description |
|----------|-------|-------------|
| `FFT(seq)` | 1 | Radix-2 Cooley–Tukey FFT; zero-padded to next power of two (min 2). Returns interleaved `[re₀, im₀, re₁, im₁, …]` of length `2N`. |
| `IFFT(interleaved)` | 1 | Inverse FFT; real time-domain samples (length `N`). |
| `DCT(seq)` / `IDCT(coeffs)` | 1 | Orthonormal DCT-II / inverse. |
| `CONV_FULL(a, b)` / `CONV1D(a, b)` | 2 | Full linear 1-D convolution; length `len(a)+len(b)-1`. Prefer **`CONV_FULL`** in scripts. |
| `CONV_SAME(a, b)` / `CONV1D_SAME(a, b)` | 2 | Centered padding; output length `len(a)`. |
| `LAPLACIAN(seq)` / `LAPLACIAN1D(seq)` | 1 | Discrete Laplacian stencil `[1,-2,1]` with zero edge padding. |
| `AD_GRAD_ADD(upstream)` | 1 | Identity backward for addition. |
| `AD_GRAD_MUL_LHS(a, b, upstream)` | 3 | ∂L/∂a for elementwise `z = a ⊙ b`. |
| `AD_GRAD_MUL_RHS(a, b, upstream)` | 3 | ∂L/∂b for elementwise multiply. |
| `AD_GRAD_RELU(x, upstream)` | 2 | ReLU backward. |
| `AD_GRAD_SIGMOID(y, upstream)` | 2 | Sigmoid backward given output `y`. |
| `AD_GRAD_CONV1D_IN(input, kernel, upstream)` | 3 | ∂L/∂input for full convolution. |
| `AD_GRAD_CONV1D_K(input, kernel, upstream)` | 3 | ∂L/∂kernel for full convolution. |
| `AD_CHAIN(local_grad, upstream)` | 2 | Elementwise chain rule `local_grad ⊙ upstream`. |

**Limits:** max transform length `1_048_576` (`MaxTransformLen`). `IFFT` requires interleaved input whose half-length is a power of two.

---

## Hessian, Wirtinger calculus, and PINN helpers

Complex values use `C(re,im)` cells or interleaved `LIST`/`VECTOR` `[re₀, im₀, …]`.

### Real Hessian (diagonal)

| Function | Arity | Description |
|----------|-------|-------------|
| `AD_HESSIAN(f2, upstream)` | 2 | General diagonal factor `f'' ⊙ upstream`. |
| `AD_HESSIAN_RELU(x, upstream)` | 2 | Zeros (ReLU has zero curvature). |
| `AD_HESSIAN_SIGMOID(y, upstream)` | 2 | Uses sigmoid output `y`. |
| `AD_HESSIAN_SQUARE(x, upstream)` | 2 | For `f(x)=x²`, factor `2·upstream`. |
| `AD_HESSIAN_TANH(y, upstream)` | 2 | Uses tanh output `y`. |
| `AD_GRAD_TANH(y, upstream)` | 2 | First-order tanh backward. |
| `AD_GRAD_MATVEC_IN(W, upstream)` | 2 | ∂L/∂x for `y = Wx`. |
| `AD_GRAD_MATVEC_W(W, x, upstream)` | 3 | ∂L/∂W flattened. |
| `AD_GRAD_MSE_PRED(pred, target)` | 2 | ∂L/∂pred for MSE. |
| `PINN_FD_CENTRAL(f_plus, f_minus, h)` | 3 | Central finite difference `(f₊−f₋)/(2h)`. |

### Wirtinger (complex)

| Function | Arity | Description |
|----------|-------|-------------|
| `AD_WIRTINGER_DZ(cartesian_grad)` | 1 | ∂L/∂z from Cartesian `(∂L/∂x, ∂L/∂y)`. |
| `AD_WIRTINGER_DZBAR(cartesian_grad)` | 1 | ∂L/∂z̄. |
| `AD_WIRTINGER_MUL_LHS(a, b, upstream)` | 3 | Wirtinger grad w.r.t. left operand of `w = a ⊙ b`. |
| `AD_WIRTINGER_MUL_RHS(a, b, upstream)` | 3 | Wirtinger grad w.r.t. right operand. |
| `AD_WIRTINGER_ABS2(z, upstream)` | 2 | For `f = |z|²`. |
| `AD_WIRTINGER_CHAIN(local_jac, upstream)` | 2 | Complex elementwise chain rule. |

---

## Computation graphs

Graphs are **DAGs** of elementwise float ops over sequences. Slot indices refer to inputs (`0…nInputs−1`) or prior node outputs (`nInputs + nodeIndex`).

### Wire format

```text
ADG[nInputs|op,in0,in1,param;op,in0,in1,param;…]
```

| Code | Op | Notes |
|------|-----|-------|
| 1 | `CONST` | Broadcast scalar `param`. |
| 2–8 | `ADD`, `MUL`, `RELU`, `SIGMOID`, `TANH`, `SCALE`, `CHAIN` | Standard elementwise ops. |
| 20–27 | `FUSED_*` | Fused microkernel ops (`MUL→RELU`, `MUL→SIGMOID`, `ADD→SCALE`, …). |

Build spec lists are flat quadruples per node: `(op, in0, in1, param)`.

| Function | Arity | Description |
|----------|-------|-------------|
| `AD_GRAPH_BUILD(n_inputs, spec)` | 2 | Build `ADG[…]` from input count and flat node spec list. |
| `AD_GRAPH_FUSE(graph)` | 1 | Table-driven fusion pass; returns optimized graph. |
| `AD_GRAPH_FORWARD(graph, inputs)` | 2 | Forward pass → output `LIST`. |
| `AD_GRAPH_CACHE(graph, inputs)` | 2 | Compact forward checkpoint for backward (`ADC2[…]`). |
| `AD_GRAPH_BACKWARD(graph, cache, upstream)` | 3 | Reverse-mode AD → input gradient list(s). |
| `AD_GRAPH_NODE_COUNT(graph)` | 1 | Node count. |

Multi-input `inputs` use a length-prefixed flat `LIST`: `[len₀, …x₀…, len₁, …x₁…, …]`.

`AD_GRAPH_FUSE` merges producer→single-consumer patterns (e.g. `MUL→RELU` → `FUSED_MUL_RELU`) using SIMD microkernels in `MathSciAutogradMicrokernels.*`. Reverse mode uses compact checkpoints (bit masks for ReLU gates, on-demand recompute for other nodes).

**Limits:** max sequence length `1_048_576`; max graph inputs `32`; max graph nodes `4096`.

```sql
WITH g AS (
  SELECT AD_GRAPH_FUSE(
    AD_GRAPH_BUILD('2', 'L[8]:3,0,1,0,4,2,0,0')
  ) AS graph
)
SELECT
  AD_GRAPH_FORWARD(graph, 'L[6]:2,1,2,2,3,4') AS y,
  AD_GRAPH_BACKWARD(
    graph,
    AD_GRAPH_CACHE(graph, 'L[6]:2,1,2,2,3,4'),
    'L[2]:1,1'
  ) AS grads
FROM g;
```

---

## ODE, SDE, and PDE solvers

Per-step explicit integrators and iterative linear/root/PDE solvers. Maximum list length: `1_048_576` (`MaxSolveLen`).

### ODE

| Function | Description |
|----------|-------------|
| `ODE_EULER(y, slope, dt)` | Explicit Euler. |
| `ODE_HEUN(y, k1, k2, dt)` | Heun / RK2. |
| `ODE_MIDPOINT(y, k_mid, dt)` | Midpoint step. |
| `ODE_RK3(y, k1, k2, k3, dt)` | Classical RK3 (SSP). |
| `ODE_RK4(y, k1, k2, k3, k4, dt)` | Classical RK4. |
| `ODE_IMPLICIT_EULER(y, lambda, dt)` | Diagonal implicit Euler for `y' = λ⊙y`. |
| `ODE_TRAPEZOID(y, k0, k1, dt)` | Explicit trapezoid. |
| `ODE_SEMI_IMPLICIT(y, k1, k2, dt)` | Staged semi-implicit. |
| `ODE_CRANK_NICOLSON(y, lambda, k, dt)` | Diagonal Crank–Nicolson. |
| `ODE_ADAMS_BASHFORTH2(y, k_curr, k_prev, dt)` | Second-order Adams–Bashforth. |
| `SOLVE_ODE(method, y, a, b, c, d, dt)` | One-step dispatcher (`EULER`, `HEUN`, `RK3`, `RK4`, …). |
| `ODE_MARCH(method, y, a, b, c, d, dt, steps)` | Repeat `SOLVE_ODE` `steps` times. |

### SDE

| Function | Description |
|----------|-------------|
| `SDE_EULER(y, drift, diffusion, z, dt)` | Euler–Maruyama. |
| `SDE_GBM(y, mu, sigma, dt, z)` | One-step geometric Brownian motion. |
| `SDE_OU(x, mu, theta, sigma, dt, z)` | Ornstein–Uhlenbeck exact transition. |
| `SDE_MILSTEIN(y, mu, sigma, dt, z)` | Scalar Milstein (strong order 1.0). |
| `SDE_MARCH(y, drift, diffusion, z, dt, steps)` | Repeat Euler–Maruyama. |

Use `RANDOM_NORMAL()` or `SETSEED` for shocks `z`.

### PDE (1-D, uniform grid)

Boundary values fixed at index `0` and `N−1` (Dirichlet-style). Grid spacing normalized (`dx = 1`).

| Function | Description |
|----------|-------------|
| `PDE_HEAT_STEP(u, alpha, dt, dx)` | Heat equation explicit FTCS step. |
| `PDE_HEAT_MARCH(u, alpha, dt, dx, steps)` | Repeat heat step. |
| `PDE_POISSON_STEP(u, f, omega)` | One Jacobi relaxation for Poisson. |
| `PDE_POISSON_GS_STEP(u, f)` | One Gauss–Seidel step. |
| `PDE_POISSON_SOR_STEP(u, f, omega)` | One SOR step. |
| `PDE_POISSON_SOLVE(u, f, omega, max_iters, tol)` | Iterate Jacobi to tolerance. |
| `PDE_ADVECTION_STEP(u, c, dt, dx)` | Upwind advection. |
| `PDE_WAVE_STEP(u_prev, u_curr, c, dt, dx)` | Leapfrog wave step. |
| `SOLVE_PDE(method, u, f, …)` | Dispatcher: `POISSON`, `POISSON_GS`, `POISSON_SOR`, `HEAT_MARCH`. |

### Iterative linear and root solvers

| Function | Description |
|----------|-------------|
| `LINEAR_JACOBI_STEP(A, x, b)` | One Jacobi iteration on dense `T[n,n]`. |
| `LINEAR_GS_STEP(A, x, b)` | One Gauss–Seidel iteration. |
| `LINEAR_SOR_STEP(A, x, b, omega)` | One SOR iteration. |
| `LINEAR_RICHARDSON_STEP(A, x, b, alpha)` | Richardson step. |
| `LINEAR_CG_SOLVE(A, x0, b, max_iters, tol)` | Conjugate gradient (SPD). |
| `SOLVE_LINEAR(method, A, x, b, …)` | Iterate to residual tolerance. |
| `ROOT_NEWTON_STEP`, `ROOT_SECANT_STEP`, `ROOT_BISECT_STEP`, `ROOT_HALLEY_STEP` | One scalar root-finding step each. |
| `SOLVE_ROOT(method, a, b, c, d)` | One-step dispatcher. |

---

## Fused DiffEq API (`DEQ_*`)

Ergonomic fused ODE integration for training and inference. One `DEQ_INTEGRATE` call replaces hundreds of SQL round-trips through `ODE_MARCH`.

| Function | Arity | Description |
|----------|-------|-------------|
| `DEQ_INTEGRATE(method, y, dt, steps, k1…k4)` | 8 | Fused explicit march (≤8192 steps). |
| `DEQ_ADAPT(method, y, t0, t1, h0, rtol, atol, k1…k4)` | 11 | Adaptive RK4 pair toward `t1`. |
| `DEQ_LINSPACE(t0, t1, n)` | 3 | Uniform time grid `L[n]:…`. |

Methods: `EULER`, `HEUN`, `RK2`, `MIDPOINT`, `RK3`, `RK4`, `DOPRI5`/`TSIT5` (fixed-step RK4 kernel), plus aliases matching `SOLVE_ODE`. `ODE_MARCH` delegates to the same fused integrator as `DEQ_INTEGRATE`.

```sql
SELECT DEQ_INTEGRATE(
  'RK4', 'L[2]:1,0', '0.01', '100',
  'L[2]:0.1,0.2', 'L[2]:0.1,0.2', 'L[2]:0.1,0.2', 'L[2]:0.1,0.2'
) AS y_end;
```

Benchmark: [`examples/benchmarks/benchmark_math_sci_perf.sql`](../examples/benchmarks/benchmark_math_sci_perf.sql) + `scripts/bench_math_sci_perf.py`.

---

## Graph and signal heat kernels

Diffusion on graphs (combinatorial Laplacian) and Gaussian heat smoothing on 1-D/2-D signals via `MathSciSignal` convolution. Results are `L[n]:…` list cells; 2-D image results use `I[w,h,c]:…`. Demo: [`examples/math_sci_heat_kernel.sql`](../examples/math_sci_heat_kernel.sql).

| Function | Arity | Description |
|----------|-------|-------------|
| `HEAT_KERNEL_GRAPH_STEP(u, edges, dt)` | 3 | One explicit step `u' = u − dt·L·u`. |
| `HEAT_KERNEL_GRAPH_MARCH(u, edges, dt, steps)` | 4 | Repeat graph step. |
| `HEAT_KERNEL_GRAPH_APPLY(u, edges, tau)` | 3 | Diffuse for diffusion time `tau` (internal sub-stepping). |
| `HEAT_KERNEL_1D(seq, sigma)` | 2 | 1-D Gaussian blur (`CONV1D_SAME` kernel). |
| `HEAT_KERNEL_2D(img, sigma)` | 2 | Separable 2-D Gaussian on `I[w,h,c]:…`. |
| `HEAT_KERNEL_GRAD_1D(seq, sigma)` | 2 | Central gradient of Gaussian-smoothed 1-D signal. |

**Graph edges:** flat `L[2E]:i₀,j₀,i₁,j₁,…` with 0-based node indices (undirected pairs). Max `4096` nodes, `65536` edges.
