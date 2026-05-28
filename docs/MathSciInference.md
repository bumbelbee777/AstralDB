# MathSci: MCTS and Bayesian inference

SQL builtins for **planning** (UCT MCTS) and **conjugate / grid Bayesian** updates. Implemented in [`sources/Database/MathSciInference.cxx`](../sources/Database/MathSciInference.cxx).

## MCTS

| Builtin | Args | Returns |
|---------|------|---------|
| `MCTS_SEARCH(rewards, priors, iterations, c)` | `L[k]` rollout means per action, `L[k]` priors, iteration count, exploration `c` | Best action index (text scalar) |
| `MCTS_UCT_PICK(q, visits, priors, parent_visits, c)` | Visit-weighted Q, visit counts, priors | One UCT pick index |

Toy **flat bandit** MCTS (no game tree expansion in SQL); suitable for benchmarking UCT selection hot paths and policy search prototypes.

## Bayesian

| Builtin | Args | Returns |
|---------|------|---------|
| `BAYES_BETA_POST(α₀, β₀, successes, failures)` | Conjugate Beta–Binomial | `L[2]:α,β` |
| `BAYES_NORMAL_POST(μ₀, τ₀, x̄, n, σ)` | Normal likelihood, known σ | `L[2]:μ_post,τ_post` |
| `BAYES_GRID_POST(log_prior, log_likelihood)` | Same-length log vectors | Normalized posterior `L[n]` |
| `BAYES_LOG_EVIDENCE(log_prior, log_likelihood)` | Same-length log vectors | log ∑ prior·likelihood (log-sum-exp) |

## Example

[`examples/math_sci_inference_nfp.sql`](../examples/math_sci_inference_nfp.sql)

## Benchmark

[`examples/benchmarks/benchmark_math_sci_inference_nfp.sql`](../examples/benchmarks/benchmark_math_sci_inference_nfp.sql) — plot via [`scripts/plot_math_sci_inference_nfp_bench.py`](../scripts/plot_math_sci_inference_nfp_bench.py).
