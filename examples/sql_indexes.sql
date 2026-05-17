-- FTS + vector indexes, improved MATCH (OR), TEXT_RANK, VECTOR_TOPK.

CREATE TABLE docs (id INT, title TEXT, body TEXT);
INSERT INTO docs VALUES
	(1, 'Alpha', 'quick brown fox'),
	(2, 'Beta', 'lazy dog sleeps'),
	(3, 'Gamma', 'quick dog jumps');

CREATE INDEX docs_body_fts ON docs (body) USING FTS;

SELECT id FROM docs WHERE body MATCH 'quick | lazy';

SELECT TEXT_RANK(body, 'quick fox') AS r FROM docs WHERE id = 1;

CREATE TABLE emb (id INT, vec VECTOR(3));
INSERT INTO emb VALUES
	(1, 'V[3]:1,0,0'),
	(2, 'V[3]:0.9,0.1,0'),
	(3, 'V[3]:0,1,0');

CREATE INDEX emb_vec ON emb (vec) USING VECTOR METRIC COSINE;

SELECT VECTOR_TOPK('emb_vec', 'V[3]:1,0,0', 2) AS neighbors;

DROP INDEX docs_body_fts;
DROP INDEX emb_vec;
