-- SQL-92 core through SQL:2003 selected features (living contract; see docs/Overview.md).
-- Predicates, scalars, strings, set ops, transactions, and temporal/JSON polish.

CREATE TABLE std (id INT, tag TEXT, n INT, amt DOUBLE);
INSERT INTO std VALUES
	(1, '', 10, 5),
	(2, 'x', 20, 15),
	(3, '', 30, 25),
	(4, 'y', 40, 35);

-- IS NULL / IS NOT NULL, BETWEEN (including NULL bound), IN, LIKE, TRUE
SELECT id FROM std WHERE tag IS NOT NULL ORDER BY id;
SELECT id FROM std WHERE n BETWEEN NULL AND 100;
SELECT id FROM std WHERE n BETWEEN 10 AND 30 ORDER BY id;
SELECT id FROM std WHERE tag IN ('x', 'y') ORDER BY id;
SELECT id FROM std WHERE n NOT IN (10, 99) ORDER BY id;
SELECT id FROM std WHERE tag LIKE '_';
SELECT id FROM std WHERE TRUE ORDER BY id;

-- NULLIF, GREATEST, LEAST, COALESCE
SELECT id, NULLIF(n, 20) AS nz, GREATEST(n, 15) AS hi, LEAST(n, 25) AS lo FROM std ORDER BY id;
SELECT id, COALESCE(NULL, tag, 'z') AS tg FROM std ORDER BY id;

-- CASE, CAST, SUBSTRING / TRIM
SELECT id,
	CASE WHEN n < 25 THEN 'low' WHEN n < 35 THEN 'mid' ELSE 'high' END AS band,
	CAST(n AS INTEGER) AS ni
FROM std ORDER BY id;

CREATE TABLE txt (s TEXT);
INSERT INTO txt VALUES ('  abc  ');
SELECT TRIM(s) AS t, SUBSTRING(s FROM 3 FOR 2) AS mid FROM txt;

-- DISTINCT, LIMIT/OFFSET comma form
SELECT DISTINCT n FROM std WHERE n <= 20 ORDER BY n;
SELECT id FROM std ORDER BY id LIMIT 1, 2;

-- Set ops (explicit columns)
CREATE TABLE a (v INT);
CREATE TABLE b (v INT);
INSERT INTO a VALUES (1), (2);
INSERT INTO b VALUES (2), (3);
SELECT v FROM a UNION SELECT v FROM b ORDER BY v;
DROP TABLE a;
DROP TABLE b;

-- Transactions and unary numeric literals
CREATE TABLE edge_nums (id INT, n INT);
INSERT INTO edge_nums VALUES (1, 0), (2, 100), (3, -7), (4, +42);
BEGIN;
UPDATE edge_nums SET n = 42 WHERE id = 1;
COMMIT;
BEGIN;
UPDATE edge_nums SET n = 99 WHERE id = 2;
ROLLBACK;
SELECT id, n FROM edge_nums ORDER BY id;
DROP TABLE edge_nums;

-- SQL:2003 temporal + JSON (subset)
CREATE TABLE events (
	id INT,
	valid_from TEXT,
	valid_to TEXT,
	kind TEXT,
	amt DOUBLE
);
INSERT INTO events VALUES
	(1, '1704067200', '1706745600', 'A', 10),
	(2, '1704153600', '', 'B', 20);
SELECT id FROM events FOR SYSTEM_TIME AS OF '1704200000' ORDER BY id;
SELECT JSON_EXTRACT('{"x":42}', 'x') AS jx FROM events WHERE id = 1;

-- IN / NOT IN subqueries (SQL-92 correlated subset)
CREATE TABLE pr (pk INT);
CREATE TABLE cr (fk INT);
INSERT INTO pr VALUES (1), (2), (3);
INSERT INTO cr VALUES (1), (3);
SELECT pk FROM pr WHERE pk IN (SELECT fk FROM cr) ORDER BY pk;
SELECT pk FROM pr WHERE pk NOT IN (SELECT fk FROM cr WHERE fk < 3) ORDER BY pk;
DROP TABLE cr;
DROP TABLE pr;

DROP TABLE events;
DROP TABLE txt;
DROP TABLE std;
