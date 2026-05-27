# MathSci NLP utilities

Text helpers for analytics-sized strings and token lists. Tokenization reuses `SQL::TextSearch::TokenizeQuery` (`sources/SQL/TextSearch.cxx`).

## Functions

| Function | Arity | Description |
|----------|-------|-------------|
| `NLP_TOKENIZE(text)` | 1 | Lowercase ASCII tokens → `LIST` cell. |
| `NLP_NGRAMS(text_or_tokens, n)` | 2 | Character n-grams from text, or token n-grams from a token `LIST`. |
| `NLP_JACCARD(a, b)` | 2 | Jaccard similarity on token sets (strings or lists). |
| `NLP_EDIT_DIST(a, b)` | 2 | Levenshtein distance (capped at 4096 chars per operand). |
| `NLP_STEM(word)` | 1 | Simple ASCII suffix stemmer (porter-lite). |

## Limits

- `NLP_EDIT_DIST`: operands longer than `4096` characters return the cap distance.
- Not a substitute for FTS `MATCH`; use indexes for large-scale search.

## Related

- [`MathSciEmbeddings.md`](MathSciEmbeddings.md) — embed token lists into vectors
- [`MathSciLmTrain.md`](MathSciLmTrain.md) — tiny LM training pipeline
- [`examples/sql_math_sci.sql`](../examples/sql_math_sci.sql), [`examples/math_sci_lm_tiny.sql`](../examples/math_sci_lm_tiny.sql)
