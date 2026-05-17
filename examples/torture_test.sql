-- AstralDB torture harness (CI-sized; heavy suites live under examples/benchmarks/).
-- Stresses lexer, WAL, FILTER_DNF, UPDATE/DELETE, DISTINCT/ORDER BY/LIMIT, transactions.

DROP TABLE IF EXISTS torture_test;

CREATE TABLE torture_test (
	id INT PRIMARY KEY NOT NULL,
	a INT NOT NULL,
	b TEXT,
	c TEXT,
	d TEXT
);

BEGIN;

INSERT INTO torture_test BULK 2500 START 1 STEP 1;

-- Narrow updates (predicate fan-out via OR + AND + BETWEEN)
UPDATE torture_test SET c = 'hot' WHERE id > 2200 AND id < 2400;
UPDATE torture_test SET d = 'zap' WHERE a BETWEEN 1 AND 20 OR id = 777;

UPDATE torture_test SET b = 'upd' WHERE id BETWEEN 50 AND 60 AND a >= 10 AND a <= 990;
UPDATE torture_test SET b = 'upd2' WHERE id BETWEEN 2400 AND 2495 AND a >= 10 AND a <= 990;

DELETE FROM torture_test WHERE b LIKE 'txt_9%' AND id IN ('901','902','903','904','905');

SELECT DISTINCT a FROM torture_test WHERE b LIKE 'upd%' ORDER BY a DESC LIMIT 50;
SELECT * FROM torture_test WHERE c = 'hot' ORDER BY id ASC LIMIT 20;

DELETE FROM torture_test WHERE id BETWEEN 2600 AND 2700;

COMMIT;

DROP TABLE IF EXISTS torture_test;
