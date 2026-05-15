-- Transactions, ORDER BY, and unary numeric literals (+/- digits / -.fraction).

CREATE TABLE edge_nums (id INTEGER, n INTEGER);

INSERT INTO edge_nums VALUES (1, 0);
INSERT INTO edge_nums VALUES (2, 100);
INSERT INTO edge_nums VALUES (3, 200);
INSERT INTO edge_nums VALUES (4, -7);
INSERT INTO edge_nums VALUES (5, +42);

SELECT id, n FROM edge_nums ORDER BY id ASC;

BEGIN;
UPDATE edge_nums SET n = 42 WHERE id = 1;
COMMIT;

BEGIN;
UPDATE edge_nums SET n = 99 WHERE id = 2;
ROLLBACK;

SELECT id, n FROM edge_nums ORDER BY id ASC;
