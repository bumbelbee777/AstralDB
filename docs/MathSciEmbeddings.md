# MathSci tokenizer embeddings

Scaffolding for **caller-supplied** embedding tables: build, lookup, batch, serialize, mean-pool, and catalog registration. Implementations live in `sources/Database/MathSciEmbeddings.*` with SIMD-backed complex/real numerics in `MathSciComplex.*`.

## Wire format

Real embedding tables use **`E[dim,count]:`**. Complex tables (interleaved re,im per slot) use **`EC[dim,count]:`** where `dim` is the number of complex components:

```text
E[2,2]:a|1,0;b|0,1
EC[2,2]:a|1,0,0,1;b|0,1,1,0
```

Rows are semicolon-separated; each row is `token|v0,v1,…`.

Vector cells accepted when building or registering embeddings:

| Prefix | Meaning |
|--------|---------|
| `L[n]:…` / `V[n]:…` | Real list / vector |
| `CV[n]:re,im,…` | Complex vector (interleaved) |
| `C(re,im)` | Single complex scalar (dim 1) |
| `T[r,c]:…` | Real matrix |
| `TC[r,c]:…` | Complex matrix (interleaved) |

## Catalog DDL

Register an embedding from an existing table (in-memory catalog, session-local):

```sql
CREATE EMBEDDING emb AS TABLE vocab (tok, vec);
DROP EMBEDDING emb;
```

Look up via catalog reference in builtins:

```sql
SELECT NLP_EMBED_LOOKUP('@emb', 'token') AS v;
SELECT NLP_EMBED_LOOKUP('catalog:emb', 'token') AS v;
```

All rows must use the same kind (all real or all complex). Tokens must be unique and non-empty.

## Functions

| Function | Arity | Description |
|----------|-------|-------------|
| `NLP_EMBED_BUILD(tokens, vectors)` | 2 | `LIST` of tokens + `MATRIX`/`TC[…]` or `LIST` of `VECTOR`/`CV[…]` → `E[…]` or `EC[…]`. |
| `NLP_EMBED_LOOKUP(table, token)` | 2 | Single vector (`@name` / `catalog:name` or inline wire cell). UNK = zero vector. |
| `NLP_EMBED_BATCH(table, tokens)` | 2 | `LIST` of vectors for each token. |
| `NLP_EMBED_SERIALIZE(table)` | 1 | Canonical wire cell. |
| `NLP_EMBED_LOAD(table)` | 1 | Parse and re-emit (validation). |
| `NLP_EMBED_FINGERPRINT(table)` | 1 | Stable hex hash for cache keys. |
| `NLP_EMBED_MEAN(tokens, table)` | 2 | Mean-pooled vector (SIMD; complex uses interleaved arithmetic). |

Shared MathSci vector ops (`COSINE_SIM`, `VECTOR_DOT`, `VECTOR_ADD`, `VECTOR_NORM`, `MATVEC`) accept real and complex cells uniformly via `MathSciComplex`.

## Pipeline

```sql
CREATE TABLE vocab (tok TEXT, vec TEXT);
INSERT INTO vocab VALUES ('a', 'CV[2]:1,0,0,1'), ('b', 'CV[2]:0,1,1,0');
CREATE EMBEDDING emb AS TABLE vocab (tok, vec);

SELECT NLP_EMBED_MEAN(
  NLP_TOKENIZE(doc),
  '@emb'
) AS doc_vec FROM docs;
```

## Limits

| Constant | Value |
|----------|-------|
| `MaxEmbeddingDim` | 4096 (complex slots; 8192 floats interleaved) |
| `MaxEmbeddingVocab` | 1_048_576 |
| `MaxEmbeddingCacheEntries` | 4096 (C++ `EmbeddingCache`, session-local) |

## C++ utilities

- `EmbeddingTable` — vocab + F32 rows + `RowsFlat` for SIMD
- `EmbeddingCatalogEntry` — DDL metadata + serialized wire in `Database::Embeddings_`
- `EmbeddingCache` — LRU for hot token vectors
- `MathSciComplex::NumericVec` / `NumericMat` — unified real/complex parse, dot, norm, matvec, cosine

## Persistence

Embedding catalogs are durable like graph catalogs:

| Mechanism | Record | When |
|-----------|--------|------|
| **WAL** | `ER\|name\|table\|tok\|vec\|isComplex\|wireB64` | Each `CREATE EMBEDDING` |
| **WAL** | `ED\|name` | Each `DROP EMBEDDING` |
| **Snapshot trailer** | `<<<ASTRAL_DB_EMBEDDINGS>>>` + serialized wire tables | Each `SyncToFile` / flush |

On reopen, the snapshot trailer restores catalogs. WAL replay (`ER` / `ED`) applies when bootstrapping from a WAL-only database (same model as graphs).

The stored **wire cell** is authoritative at registration time; changing source table rows does not auto-refresh the catalog until you `DROP` and `CREATE` again.

## Non-goals

- No ONNX/GGUF or external model loaders
- Does not replace `VECTOR` indexes; run `VECTOR_TOPK` on pooled document vectors for ANN search

## Related

- `docs/MathSciNlp.md`, `docs/MathSciClassify.md`
- `examples/sql_math_sci.sql`
