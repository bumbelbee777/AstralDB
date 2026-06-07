# MathSci inference: MCTS, Bayesian, and macro Fokker–Planck

SQL builtins for **planning** (UCT MCTS), **conjugate and grid Bayesian** updates, and **neural macro Fokker–Planck** (NFP) distribution dynamics. Implementations: `sources/Database/MathSci/MathSciInference.*`, `MathSciFokkerPlanck.*`.

For MLP models and PINN workflows see [`MathSciMl.md`](MathSciMl.md). For classical ODE/SDE/PDE solvers see [`MathSciCore.md`](MathSciCore.md).

Demo: [`examples/math_sci_inference_nfp.sql`](../examples/math_sci_inference_nfp.sql). Benchmark: [`examples/benchmarks/benchmark_math_sci_inference_nfp.sql`](../examples/benchmarks/benchmark_math_sci_inference_nfp.sql) — plot via `python scripts/plot_math_sci_inference_nfp_bench.py`.

---

## MCTS

Flat **bandit-style** MCTS (no game-tree expansion in SQL); suitable for UCT selection benchmarks and policy-search prototypes.

| Builtin | Args | Returns |
|---------|------|---------|
| `MCTS_SEARCH(rewards, priors, iterations, c)` | `L[k]` rollout means, `L[k]` priors, iteration count, exploration `c` | Best action index (text scalar) |
| `MCTS_UCT_PICK(q, visits, priors, parent_visits, c)` | Visit-weighted Q, visit counts, priors | One UCT pick index |

---

## Bayesian inference

| Builtin | Args | Returns |
|---------|------|---------|
| `BAYES_BETA_POST(α₀, β₀, successes, failures)` | Conjugate Beta–Binomial | `L[2]:α,β` |
| `BAYES_NORMAL_POST(μ₀, τ₀, x̄, n, σ)` | Normal likelihood, known σ | `L[2]:μ_post,τ_post` |
| `BAYES_GRID_POST(log_prior, log_likelihood)` | Same-length log vectors | Normalized posterior `L[n]` |
| `BAYES_LOG_EVIDENCE(log_prior, log_likelihood)` | Same-length log vectors | log ∑ prior·likelihood (log-sum-exp) |

---

## Neural macro Fokker–Planck (NFP)

Forward Kolmogorov step for a **wealth distribution** on a 1-D grid with quant-macro features:

- **Borrowing barrier** (reflecting/absorbing mass at `a_min`)
- **Mean-field capital feedback** (aggregate `K` vs clearing `K*`)
- **State-dependent diffusion** (idiosyncratic risk per grid point)
- **Neural drift correction** `L[n]` (MLP residual from `PREDICT` / training loop)

This is the PDE behind heterogeneous-agent (HANK) distribution dynamics; the SQL surface is one fused step or multi-step march.

| Builtin | Args | Returns |
|---------|------|---------|
| `NFP_MACRO_STEP(g, grid, drift, diffusion, neural_corr, agg_K, K_star, barrier_a, dt)` | Density and grid `L[n]`, same-length coeffs | Updated density `L[n]` |
| `NFP_MACRO_MARCH(…, steps)` | Same + integer steps | Density after `steps` explicit steps |
| `NFP_MACRO_MOMENTS(g, grid)` | Density + grid | `L[4]: mass, mean_wealth, variance, gini_proxy` |

Conservative finite-volume explicit step with CFL clamp; mass renormalized each step.

### Workflow with PINNs / MLP

1. `PREDICT(mdl, …)` → savings drift per collocation point.
2. Map to grid → `neural_corr` list.
3. `NFP_MACRO_MARCH` advances `g`; read `NFP_MACRO_MOMENTS` for market-clearing residual.
