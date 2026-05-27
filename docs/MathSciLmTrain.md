# Tiny language-model training in SQL

AstralDB does not ship a separate training runtime. A **toy next-token pipeline** is expressed with existing **NLP**, **embedding catalog**, **classifier**, and **vector** builtins—suitable for short corpora and demos, not production LLM training.

## Building blocks

| Piece | Builtin / DDL |
|-------|----------------|
| Tokenization | `NLP_TOKENIZE(text)` |
| Vocabulary vectors | `NLP_EMBED_BUILD`, `CREATE EMBEDDING … AS TABLE` |
| Scoring | `VECTOR_DOT`, `MATVEC`, `CLASSIFY_LOGISTIC`, `CLASSIFY_ARGMAX` |
| Loss | `MSE_LOSS`, `LOGISTIC` |
| Update | `VECTOR_ADD` (manual SGD step in SQL) |

Scalar builtins cannot nest in a single projection argument; compose with **CTEs** or multiple columns (see [`MathSciSignal.md`](MathSciSignal.md)).

## Pipeline (conceptual)

1. Store a short story in a `corpus` table.
2. Build a small vocab table and register `CREATE EMBEDDING story_emb AS TABLE vocab (tok, vec)`.
3. Insert bigram training pairs `(ctx_tok, next_tok)` derived from the story.
4. Keep a weight vector `w` in a `lm_state` table; unroll a few epochs with forward → loss → `VECTOR_ADD` update.
5. **Inference:** tokenize a prompt, `NLP_EMBED_LOOKUP` for context, score all vocab dots, `CLASSIFY_ARGMAX` for the predicted next token.

## Runnable demo

[`examples/math_sci_lm_tiny.sql`](../examples/math_sci_lm_tiny.sql) — trains on an excerpt of *The Fox and the Grapes* and runs one inference query.

## Related

- [`MathSciNlp.md`](MathSciNlp.md), [`MathSciEmbeddings.md`](MathSciEmbeddings.md), [`MathSciClassify.md`](MathSciClassify.md)
- [`examples/sql_math_sci.sql`](../examples/sql_math_sci.sql)
