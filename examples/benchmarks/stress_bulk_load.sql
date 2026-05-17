-- Off-CI bulk load + scan (run: scripts/run_torture_isolated.ps1 -SqlPath examples/benchmarks/stress_bulk_load.sql)

DROP TABLE IF EXISTS stress_bulk;

CREATE TABLE stress_bulk (id INT, a INT, b TEXT, c TEXT, d TEXT);

BEGIN;
INSERT INTO stress_bulk BULK 150000 START 1 STEP 1;
COMMIT;

SELECT id, a, b FROM stress_bulk WHERE id BETWEEN 50000 AND 50100 ORDER BY id ASC LIMIT 200;
SELECT DISTINCT a FROM stress_bulk WHERE b LIKE 'txt_1%' ORDER BY a DESC LIMIT 500;

DROP TABLE IF EXISTS stress_bulk;
