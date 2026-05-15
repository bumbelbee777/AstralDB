-- Parser + VM smoke: predicates, DISTINCT (full-row), LIMIT/OFFSET comma form, DDL.
CREATE TABLE cmp (id INTEGER);
INSERT INTO cmp VALUES (1), (1), (2), (3);

SELECT DISTINCT id FROM cmp WHERE id <= 2 ORDER BY id ASC;

DROP TABLE cmp;

CREATE TABLE lim (seq INTEGER);
INSERT INTO lim VALUES (10), (20), (30), (40);

SELECT seq FROM lim ORDER BY seq ASC LIMIT 1, 2;

DROP TABLE lim;
