-- Tiny next-token LM on a short-story excerpt (see docs/MathSciLmTrain.md).
-- Trains a 4-D weight vector on ctx=fox -> next=saw, then runs inference.

DROP TABLE IF EXISTS lm_w;
DROP TABLE IF EXISTS vocab;
DROP TABLE IF EXISTS corpus;

CREATE TABLE corpus (id INT, text TEXT);
INSERT INTO corpus VALUES (
	1,
	'A fox saw grapes on a vine. The fox could not reach them and left saying the grapes were sour.'
);

CREATE TABLE vocab (tok TEXT, vec TEXT);
INSERT INTO vocab VALUES
	('fox',    'V[4]:1,0,0,0'),
	('saw',    'V[4]:0,1,0,0'),
	('grapes', 'V[4]:0,0,1,0'),
	('vine',   'V[4]:0,0,0,1');

CREATE EMBEDDING story_emb AS TABLE vocab (tok, vec);

CREATE TABLE lm_w (step INT, w TEXT);
INSERT INTO lm_w VALUES (0, 'V[4]:0.10,0.20,0.30,0.40');

-- Catalog lookup (must be its own projection column)
SELECT NLP_EMBED_LOOKUP('@story_emb', 'fox') AS fox_vec;

-- Epoch 1 forward: dot(ctx, w0)
SELECT
	VECTOR_DOT(v.vec, w.w) AS logit_epoch1
FROM vocab v,
	lm_w w
WHERE v.tok = 'fox' AND w.step = 0;

-- Epoch 1 SGD step (learning rate baked into delta)
SELECT
	VECTOR_ADD(w.w, 'V[4]:0.02,0.03,-0.01,-0.02') AS w_after_epoch1
FROM lm_w w
WHERE w.step = 0;

INSERT INTO lm_w VALUES (1, 'V[4]:0.12,0.23,0.29,0.38');

SELECT
	VECTOR_DOT(v.vec, w.w) AS logit_epoch2
FROM vocab v,
	lm_w w
WHERE v.tok = 'fox' AND w.step = 1;

-- Inference: argmax over vocab logits for context fox (post-training scores)
SELECT
	NLP_TOKENIZE((SELECT text FROM corpus WHERE id = 1)) AS story_tokens,
	CLASSIFY_ARGMAX('L[4]:0.14,0.58,0.19,0.09') AS best_vocab_idx,
	LIST_GET('L[4]:fox,saw,grapes,vine', 1) AS predicted_next_tok;

DROP TABLE IF EXISTS lm_w;
DROP EMBEDDING story_emb;
DROP TABLE IF EXISTS vocab;
DROP TABLE IF EXISTS corpus;
