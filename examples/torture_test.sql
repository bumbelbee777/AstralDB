-- AstralDB torture harness (CI-sized; heavy suites live under examples/benchmarks/).
-- Lazy BULK + lazy DML overlays; tail queries use CTEs for metadata-friendly shapes.

DROP TABLE IF EXISTS torture_test;

CREATE TABLE torture_test (
	id INT PRIMARY KEY NOT NULL,
	a INT NOT NULL,
	b TEXT,
	c TEXT,
	d TEXT
);

BEGIN;

INSERT INTO torture_test BULK 4096 START 1 STEP 1;
INSERT INTO torture_test BULK 4096 START 4097 STEP 1;

UPDATE torture_test SET c = 'hot' WHERE id > 6000 AND id < 7500;
UPDATE torture_test SET d = 'zap' WHERE a BETWEEN 1 AND 20 OR id = 777 OR id = 1337;
UPDATE torture_test SET b = 'upd' WHERE id BETWEEN 50 AND 120 AND a >= 10 AND a <= 990;
UPDATE torture_test SET b = 'upd2' WHERE id BETWEEN 7000 AND 7800 AND a >= 10 AND a <= 990;
UPDATE torture_test SET c = 'warm' WHERE b LIKE 'txt_5%' AND a BETWEEN 100 AND 800;

DELETE FROM torture_test WHERE b LIKE 'txt_9%' AND id IN ('901','902','903','904','905');
DELETE FROM torture_test WHERE id BETWEEN 7900 AND 8000;

WITH upd AS (
	SELECT a
	FROM torture_test
	WHERE b LIKE 'upd%'
)
SELECT DISTINCT a
FROM upd
ORDER BY a DESC
LIMIT 40;

WITH hot AS (
	SELECT id, a, b, c, d
	FROM torture_test
	WHERE c = 'hot'
)
SELECT id, a, b, c, d
FROM hot
ORDER BY id ASC
LIMIT 15;

DELETE FROM torture_test WHERE id BETWEEN 7500 AND 7600;

INSERT INTO torture_test BULK 4096 START 8193 STEP 1;
UPDATE torture_test SET d = 'late' WHERE id BETWEEN 8193 AND 9000;

COMMIT;

DROP TABLE IF EXISTS torture_test;
