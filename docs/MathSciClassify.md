# MathSci classifiers

Lightweight **inference** builtins for linear and multi-class scoring. Training stays in SQL/CTEs; implementations live in `sources/Database/MathSciClassify.*`.

## Functions

| Function | Arity | Description |
|----------|-------|-------------|
| `CLASSIFY_LINEAR(weights, features, bias)` | 3 | Dot product + bias vs threshold 0 → `'0'` or `'1'`. |
| `CLASSIFY_LOGISTIC(weights, features)` | 2 | `σ(w·x)` as a numeric probability. |
| `CLASSIFY_ARGMAX(values)` | 1 | Index of the maximum element in a numeric list/vector. |
| `CLASSIFY_ONE_VS_REST(features, weight_lists)` | 2 | Argmax over dot scores; second arg is a `LIST` of weight lists. |

## Notes

- `weights` and `features` accept `L[n]:…` or `V[n]:…` cells (same rules as `MEAN`, `COSINE_SIM`).
- Does not duplicate scalar `LOGISTIC` / `SOFTMAX`; use those for element-wise transforms on a single list.

## Related

- `examples/sql_math_sci.sql`
- `docs/MathSciNlp.md`, `docs/MathSciEmbeddings.md`
