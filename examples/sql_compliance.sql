-- Deprecated: use examples/sql92_03_coverage.sql (parser + VM smoke retained here for CI grep stability).
CREATE TABLE cmp (id INTEGER);
INSERT INTO cmp VALUES (1), (1), (2);
SELECT DISTINCT id FROM cmp WHERE id <= 2 ORDER BY id;
DROP TABLE cmp;
