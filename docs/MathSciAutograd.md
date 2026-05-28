# MathSci autograd: Hessian and Wirtinger calculus

Second-order and **complex Wirtinger** automatic differentiation primitives for `MathSci`, implemented in `sources/Database/MathSciAutograd.*`.

## Complex wire format

Complex values use either:

- `C(re,im)` cells (`COMPLEX` type), or
- `LIST` / `VECTOR` cells with interleaved `[re₀, im₀, re₁, im₁, …]`.

Wirtinger builtins accept the same shapes as `FFT` output (interleaved) or `C(...)` literals.

## Hessian (real, diagonal)

Diagonal contributions for elementwise maps \(y = f(x)\): `AD_HESSIAN(second_deriv, upstream)` returns `second_deriv ⊙ upstream`.

| Function | Arity | Description |
|----------|-------|-------------|
| `AD_HESSIAN(f2, upstream)` | 2 | General diagonal Hessian factor `f'' ⊙ upstream`. |
| `AD_HESSIAN_RELU(x, upstream)` | 2 | Zeros (ReLU has zero curvature). |
| `AD_HESSIAN_SIGMOID(y, upstream)` | 2 | Uses sigmoid output `y` and upstream ∂L/∂y′. |
| `AD_HESSIAN_SQUARE(x, upstream)` | 2 | For \(f(x)=x^2\), diagonal factor `2·upstream`. |
| `AD_HESSIAN_TANH(y, upstream)` | 2 | Uses tanh output `y` and upstream ∂L/∂y′. |

## First-order PINN helpers (real)

| Function | Arity | Description |
|----------|-------|-------------|
| `AD_GRAD_TANH(y, upstream)` | 2 | ∂L/∂x for \(y=\tanh(x)\) given `y` and upstream. |
| `AD_GRAD_MATVEC_IN(W, upstream)` | 2 | ∂L/∂x for \(y=Wx\); `upstream` length = rows of `W`. |
| `AD_GRAD_MATVEC_W(W, x, upstream)` | 3 | ∂L/∂W flattened (list) for \(y=Wx\). |
| `AD_GRAD_MSE_PRED(pred, target)` | 2 | ∂L/∂pred for mean squared error. |
| `PINN_FD_CENTRAL(f_plus, f_minus, h)` | 3 | Central finite difference \((f_+-f_-)/(2h)\). |

## Wirtinger (complex)

Treat \(z = x + iy\). Cartesian gradients \((\partial L/\partial x, \partial L/\partial y)\) convert to Wirtinger coordinates:

| Function | Arity | Description |
|----------|-------|-------------|
| `AD_WIRTINGER_DZ(cartesian_grad)` | 1 | \(\partial L/\partial z = \tfrac12(\partial L/\partial x - i\,\partial L/\partial y)\). |
| `AD_WIRTINGER_DZBAR(cartesian_grad)` | 1 | \(\partial L/\partial \bar z = \tfrac12(\partial L/\partial x + i\,\partial L/\partial y)\). |
| `AD_WIRTINGER_MUL_LHS(a, b, upstream)` | 3 | Wirtinger grad w.r.t. left operand of \(w = a \odot b\). |
| `AD_WIRTINGER_MUL_RHS(a, b, upstream)` | 3 | Wirtinger grad w.r.t. right operand. |
| `AD_WIRTINGER_ABS2(z, upstream)` | 2 | For \(f = \|z\|^2\). |
| `AD_WIRTINGER_CHAIN(local_jac, upstream)` | 2 | Complex elementwise chain rule. |

Compose with first-order `AD_GRAD_*` and `AD_CHAIN` from `docs/MathSciSignal.md` for full pipelines.

## Limits

Maximum sequence length: `1_048_576` (`MaxAutogradLen`).

## Related

- `docs/MathSciSignal.md` — FFT, convolution, real autograd
- `docs/MathSciSolves.md` — ODE/PDE/SDE solvers
