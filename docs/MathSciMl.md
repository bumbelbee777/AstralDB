# MathSci ML: models, NLP, and PINN

Feed-forward **MLP models**, **memory-efficient weight formats**, **NLP and embedding catalogs**, **classifiers**, **tiny LM training**, and **physics-informed neural network (PINN)** workflows—all in SQL without a separate training runtime. Implementations: `MathSciModel.*`, `MathSciMl.*`, `MathSciNlp.*`, `MathSciEmbeddings.*`, `MathSciClassify.*`.

For signal processing, autograd graphs, and classical solvers see [`MathSciCore.md`](MathSciCore.md). For MCTS, Bayesian updates, and macro Fokker–Planck see [`MathSciInference.md`](MathSciInference.md).

**Parser note:** do not nest scalar builtins in one argument; use CTEs or split `SELECT`s. Store model cells in a column named **`mdl`** (or any name other than **`model`**, which is reserved).

---

## Model cells and `PREDICT`

Compact binary MLP weights for PINN and classifier demos.

### Wire formats

| Prefix | Meaning |
|--------|---------|
| `MB1:` + hex | Version-1 binary blob (canonical) |
| `M[act,…]:` + `T[r,c]:…` | Human-readable text layers |
| `LW[n]:` + `T…;T…` | **Build** weight list (semicolons separate `T[r,c]` cells) |

| Function | Arity | Description |
|----------|-------|-------------|
| `MATHSCI_MODEL_BUILD(activations, weights)` | 2 | `L[k]:sigmoid,linear` + `LW[k]:T…;T…` → `MB1:…` |
| `MATHSCI_MODEL_SERIALIZE(model)` | 1 | Canonical binary cell |
| `MATHSCI_MODEL_IMPORT(model)` / `MATHSCI_MODEL_LOAD(model)` | 1 | Parse and validate → `MB1:…` |
| `MATHSCI_MODEL_FINGERPRINT(model)` | 1 | Stable hex hash |
| `PREDICT(model, features)` | 2 | Forward pass → `L[n]:…` (LRU cache, SIMD `matvec`) |

Activations per layer: `linear`, `sigmoid`, `tanh`, `relu`.

```sql
SELECT PREDICT(
  MATHSCI_MODEL_BUILD(
    'L[2]:sigmoid,linear',
    'LW[2]:T[4,2]:0.5,-0.2,0.1,0.3,0.2,0.4,-0.1,0.2;T[2,4]:0.3,0.1,-0.2,0.4,0.2,-0.1,0.3,0.2'
  ),
  'L[2]:0.5,0.2'
) AS y;
```

---

## ML memory optimizations

Quantization, mixed precision, magnitude pruning, and sinusoidal positional encoding. Real and complex tensors share the same builtins; dtypes follow NumPy naming.

### Dtype wire formats

| Dtype | Cell prefix | Notes |
|-------|-------------|-------|
| `float32` | `L[n]:`, `V[n]:`, `T[r,c]:` | Real vectors/matrices |
| `float64` | `D[n]:` | Real FP64 list |
| `float16` | `F16[n]:` | IEEE half bit patterns |
| `int8` / `int4` | `Q8[n]:` / `Q4[n]:` | Symmetric quantized real |
| `complex64` / `complex128` / `complex16` | `CV[n]:`, `TC[r,c]:`, `CD[n]:`, `CH[n]:` | Interleaved re,im |
| `complexint8` / `complexint4` | `Q8C[n]:`, `Q4C[n]:`, … | Quantized complex |

| Function | Description |
|----------|-------------|
| `ML_DTYPE(cell)` | Returns dtype name string |
| `ML_CAST(cell, dtype)` | Cast between supported dtypes |
| `ML_QUANTIZE_INT8(seq)` / `ML_DEQUANTIZE_INT8(q8)` | Per-tensor symmetric INT8 |
| `ML_QUANTIZE_MODEL(model)` / `ML_DEQUANTIZE_MODEL(mq8)` | `MB1:` ↔ `MQ8:` (~4× smaller) |
| `ML_PREDICT_QUANT(mq8, input)` | Forward on quantized weights |
| `ML_MIXED_PREC(seq, mode)` / `ML_MIXED_PREC_MODEL(model, mode)` | FP16/FP32/FP64 or complex16/64/128 |
| `ML_PRUNE(seq, threshold)` / `ML_PRUNE_MODEL(model, threshold)` | Zero small-magnitude weights |
| `ML_POSENC(pos, dim)` / `ML_POSENC_SEQUENCE(len, dim)` | Sinusoidal positional encoding |
| `ML_POSENC_ADD(seq, dim)` | Add PE to token sequence |
| `ML_POSENC_C(pos, slots)` | Complex PE: `exp(i·θ)` per slot |

---

## NLP utilities

Text helpers for analytics-sized strings. Tokenization reuses `SQL::TextSearch::TokenizeQuery`.

| Function | Arity | Description |
|----------|-------|-------------|
| `NLP_TOKENIZE(text)` | 1 | Lowercase ASCII tokens → `LIST`. |
| `NLP_NGRAMS(text_or_tokens, n)` | 2 | Character or token n-grams. |
| `NLP_JACCARD(a, b)` | 2 | Jaccard similarity on token sets. |
| `NLP_EDIT_DIST(a, b)` | 2 | Levenshtein distance (cap 4096 chars per operand). |
| `NLP_STEM(word)` | 1 | Simple ASCII suffix stemmer. |

Not a substitute for FTS `MATCH` on large corpora.

---

## Vision and OCR features

Image load, preprocessing, and lightweight HOG-style features for OCR/analytics pipelines. Implementation: `MathSciVision.*` (stb_image, `STBI_MAX_DIMENSIONS=4096`). Demo: [`examples/math_sci_ocr_demo.sql`](../examples/math_sci_ocr_demo.sql).

### Image wire format

```text
I[w,h,c]:p₀,p₁,…,p_{w·h·c−1}
```

Pixels are **normalized floats** in `[0, 1]` (row-major, channel-last). `IMG_LOAD` accepts raw byte cells or hex blobs with optional `H:` prefix.

| Function | Arity | Description |
|----------|-------|-------------|
| `IMG_LOAD(blob)` | 1 | Decode PNG/JPEG/BMP/… via stb_image → `I[w,h,c]:…`. |
| `IMG_GRAY(img)` | 1 | RGB→luminance or pass-through single channel. |
| `IMG_RESIZE(img, new_w, new_h)` | 3 | Bilinear resize. |
| `IMG_PATCHES(img, patch_w, patch_h, stride)` | 4 | Extract patches → `LIST` of `I[patch_w,patch_h,c]:…`. |
| `IMG_HOG_LITE(img, cell_size)` | 2 | Per-cell gradient-orientation histogram features → `L[n]:…`. |
| `IMG_FLATTEN(img)` | 1 | Row-major pixel vector → `L[w·h·c]:…`. |

---

## Embedding catalogs

Caller-supplied embedding tables: build, lookup, batch, serialize, mean-pool, and catalog registration.

### Wire format

Real tables: **`E[dim,count]:`**. Complex (interleaved re,im): **`EC[dim,count]:`**.

```text
E[2,2]:a|1,0;b|0,1
EC[2,2]:a|1,0,0,1;b|0,1,1,0
```

### Catalog DDL

```sql
CREATE EMBEDDING emb AS TABLE vocab (tok, vec);
DROP EMBEDDING emb;
SELECT NLP_EMBED_LOOKUP('@emb', 'token') AS v;
```

| Function | Description |
|----------|-------------|
| `NLP_EMBED_BUILD(tokens, vectors)` | Build `E[…]` or `EC[…]` from token list + matrix |
| `NLP_EMBED_LOOKUP(table, token)` | Single vector (`@name`, `catalog:name`, or inline wire) |
| `NLP_EMBED_BATCH(table, tokens)` | List of vectors |
| `NLP_EMBED_SERIALIZE(table)` / `NLP_EMBED_LOAD(table)` | Canonical wire / validation |
| `NLP_EMBED_FINGERPRINT(table)` | Stable hex hash |
| `NLP_EMBED_MEAN(tokens, table)` | Mean-pooled document vector (SIMD) |

Catalogs persist via WAL (`ER|`/`ED|` records) and snapshot trailer (`<<<ASTRAL_DB_EMBEDDINGS>>>`). The stored wire cell is authoritative at registration time.

**Limits:** max dim 4096 (complex slots); max vocab `1_048_576`; LRU cache 4096 entries.

---

## Classifiers

Lightweight inference builtins; training stays in SQL/CTEs.

| Function | Description |
|----------|-------------|
| `CLASSIFY_LINEAR(weights, features, bias)` | Dot + bias vs 0 → `'0'` or `'1'`. |
| `CLASSIFY_LOGISTIC(weights, features)` | `σ(w·x)` probability. |
| `CLASSIFY_ARGMAX(values)` | Index of maximum element. |
| `CLASSIFY_ONE_VS_REST(features, weight_lists)` | Argmax over one-vs-rest dot scores. |

Use scalar `LOGISTIC` / `SOFTMAX` for element-wise transforms on a single list.

---

## Tiny language-model training

No separate training runtime—a toy next-token pipeline from existing builtins:

1. Store corpus; `NLP_TOKENIZE` documents.
2. Build vocab + `CREATE EMBEDDING … AS TABLE`.
3. Derive bigram pairs `(ctx_tok, next_tok)`.
4. Keep weight vector `w` in a state table; unroll epochs with forward → `MSE_LOSS` → `VECTOR_ADD` (manual SGD).
5. Inference: embed context, optionally `ML_POSENC_ADD`, score vocab dots, `CLASSIFY_ARGMAX`.

Demo: [`examples/math_sci_lm_tiny.sql`](../examples/math_sci_lm_tiny.sql) (*The Fox and the Grapes* excerpt).

---

## Physics-informed neural networks (PINN)

PINNs minimize a **PDE residual** at collocation points plus boundary/initial penalties. Compose finite-difference derivatives of a small MLP with `MSE_LOSS`, `AD_CHAIN`, and Wirtinger builtins from [`MathSciCore.md`](MathSciCore.md).

### Pattern

1. **Collocation table** — rows `(x)` or `(x,y,z)` or `(x,t)`.
2. **Weights** — `W1`, `W2` as `T[r,c]:…` cells.
3. **Forward** — `h = SIGMOID(MATVEC(W1, inputs))`, `out = MATVEC(W2, h)`.
4. **Derivatives** — evaluate at `x ± ε`; build `∂u/∂x`, `∇²u`, etc.
5. **Residual** — `MSE_LOSS(residual, zeros)`.
6. **Training** — unroll gradient steps with `AD_GRAD_*`, `VECTOR_ADD`.
7. **Model I/O** — `MATHSCI_MODEL_BUILD` → `MB1:…`; `PREDICT(mdl, inputs)`.

### Demos and benchmarks

| Script | Model |
|--------|--------|
| [`examples/math_sci_pinn_tdse_1d.sql`](../examples/math_sci_pinn_tdse_1d.sql) | 1-D time-dependent Schrödinger (complex `CV[]`, Wirtinger) |
| [`examples/math_sci_pinn_navier_stokes_3d.sql`](../examples/math_sci_pinn_navier_stokes_3d.sql) | Steady 3-D Navier–Stokes-style residual |

| Benchmark harness | Plot script |
|-------------------|-------------|
| [`examples/benchmarks/benchmark_pinn_tdse_1d.sql`](../examples/benchmarks/benchmark_pinn_tdse_1d.sql) | `python scripts/plot_math_sci_pinn_bench.py` |
| [`examples/benchmarks/benchmark_pinn_navier_stokes_3d.sql`](../examples/benchmarks/benchmark_pinn_navier_stokes_3d.sql) | same |

Classical PDE steppers (`PDE_HEAT_*`, `PDE_POISSON_*`, …) are in [`MathSciCore.md`](MathSciCore.md).
