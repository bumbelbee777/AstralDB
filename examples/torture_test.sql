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

INSERT INTO torture_test BULK 6000 START 1 STEP 1;

-- Narrow updates (predicate fan-out via OR + AND + BETWEEN)
UPDATE torture_test SET c = 'hot' WHERE id > 5200 AND id < 5800;
UPDATE torture_test SET d = 'zap' WHERE a BETWEEN 1 AND 20 OR id = 777 OR id = 1337;

UPDATE torture_test SET b = 'upd' WHERE id BETWEEN 50 AND 120 AND a >= 10 AND a <= 990;
UPDATE torture_test SET b = 'upd2' WHERE id BETWEEN 5400 AND 5950 AND a >= 10 AND a <= 990;
UPDATE torture_test SET c = 'warm' WHERE b LIKE 'txt_5%' AND a BETWEEN 100 AND 800;

DELETE FROM torture_test WHERE b LIKE 'txt_9%' AND id IN ('901','902','903','904','905','906','907');
DELETE FROM torture_test WHERE id BETWEEN 5900 AND 6000;

SELECT DISTINCT a FROM torture_test WHERE b LIKE 'upd%' ORDER BY a DESC LIMIT 50;
SELECT * FROM torture_test WHERE c = 'hot' ORDER BY id ASC LIMIT 20;

DELETE FROM torture_test WHERE id BETWEEN 5600 AND 5700;

INSERT INTO torture_test BULK 400 START 7000 STEP 1;
UPDATE torture_test SET d = 'late' WHERE id BETWEEN 7000 AND 7200;

COMMIT;

DROP TABLE IF EXISTS torture_test;
