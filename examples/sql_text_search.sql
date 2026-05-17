-- Text search via WHERE col MATCH 'terms', scalars TEXT_CONTAINS / MATCH_AGAINST.

CREATE TABLE docs (id INTEGER PRIMARY KEY, title TEXT, body TEXT);
INSERT INTO docs VALUES (1, 'Intro', 'alpha beta gamma');
INSERT INTO docs VALUES (2, 'Deep', 'delta epsilon zeta');
INSERT INTO docs VALUES (3, 'Mix', 'alpha zeta');

SELECT id, title FROM docs WHERE body MATCH 'alpha zeta';
SELECT id, TEXT_CONTAINS(body, 'epsilon') AS has_eps FROM docs;
SELECT id, MATCH_AGAINST(body, '"alpha beta"') AS phrase FROM docs WHERE id = 1;
