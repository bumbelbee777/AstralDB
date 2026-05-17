# MathSci signal processing and autograd

SIMD-accelerated spectral and convolution primitives exposed as SQL scalar builtins via `MathSci`. Implementations live in `sources/Database/MathSciSignal.*` and reuse `AstralDB::Simd` float kernels from `sources/IO/SIMD.*`.

## SQL builtins

All sequence arguments accept `LIST(...)` wire cells or `VECTOR(...)` payloads (same rules as `MEAN`, `COSINE_SIM`, etc.). Results are returned as `LIST` cells unless noted.

Scalar builtins cannot be nested inside one another in the current SELECT-list parser (each argument must be a literal, `NULL`, or column reference). Compose results in separate projection columns, CTEs, or multiple statements.

| Function | Arity | Description |
|----------|-------|-------------|
| `FFT(seq)` | 1 | Radix-2 Cooley–Tukey FFT. Input is zero-padded to the next power of two (minimum 2). Returns interleaved `[re₀, im₀, re₁, im₁, …]` of length `2N`. |
| `IFFT(interleaved)` | 1 | Inverse FFT on interleaved complex data; returns real time-domain samples (length `N`). |
| `DCT(seq)` | 1 | Orthonormal DCT-II along the sequence. |
| `IDCT(coeffs)` | 1 | Inverse DCT-II (reconstructs the original signal within floating tolerance). |
| `CONV_FULL(a, b)` / `CONV1D(a, b)` | 2 | Full linear 1-D convolution; output length `len(a) + len(b) - 1`. Prefer **`CONV_FULL`** in scripts (some builds tokenize `CONV1D` as three tokens). |
| `CONV_SAME(a, b)` / `CONV1D_SAME(a, b)` | 2 | Convolution with centered padding; output length `len(a)`. |
| `LAPLACIAN(seq)` / `LAPLACIAN1D(seq)` | 1 | Discrete Laplacian stencil `[1, -2, 1]` with zero edge padding. |
| `AD_GRAD_ADD(upstream)` | 1 | Identity backward for addition: returns `upstream`. |
| `AD_GRAD_MUL_LHS(a, b, upstream)` | 3 | ∂L/∂a for elementwise `z = a * b`. |
| `AD_GRAD_MUL_RHS(a, b, upstream)` | 3 | ∂L/∂b for elementwise `z = a * b`. |
| `AD_GRAD_RELU(x, upstream)` | 2 | ∂L/∂x for ReLU. |
| `AD_GRAD_SIGMOID(y, upstream)` | 2 | ∂L/∂x given sigmoid output `y` and upstream ∂L/∂y. |
| `AD_GRAD_CONV1D_IN(input, kernel, upstream)` | 3 | ∂L/∂input for full `CONV1D`. |
| `AD_GRAD_CONV1D_K(input, kernel, upstream)` | 3 | ∂L/∂kernel for full `CONV1D`. |
| `AD_CHAIN(local_grad, upstream)` | 2 | Elementwise chain rule `local_grad * upstream`. |

## Implementation notes

- **FFT / IFFT:** In-place radix-2 on `float` buffers; butterfly stages use scalar twiddle factors (lengths are typically modest in SQL workloads). `Simd::ScaleF32` applies the `1/N` IFFT normalization.
- **DCT / IDCT:** Direct O(n²) cosine synthesis with orthonormal scaling; suitable for analytics-sized lists (cap `2²⁰` samples per transform).
- **Convolution:** Full mode uses explicit accumulation; autograd w.r.t. input uses flipped-kernel full convolution on the upstream gradient.
- **Laplacian:** Second-difference operator with zero exterior samples.
- **Autograd:** Tape-free explicit partials for composing manual backward passes in SQL (e.g. `AD_CHAIN(AD_GRAD_RELU(x, g), h)`).

## Limits

- Maximum transform length: `1_048_576` samples (`MaxTransformLen` in `MathSciSignal.cxx`).
- `IFFT` requires interleaved input whose half-length is a power of two (output of `FFT`).

## Examples

See `examples/sql_math_sci.sql` and tests `MathSci: signal FFT conv Laplacian autograd` in `tests/AstralDB.Tests.cxx`.

## Related code

- `sources/Database/MathSci.cxx` — builtin table and `EvalScalar` dispatch
- `sources/SQL/SQL.hxx` — `ScalarSqlFn` enum entries
- `sources/IO/SIMD.hxx` — `AddF32`, `MulF32`, `SubF32`, `ScaleF32`, `ComplexMulAccumulateF32`, etc.
