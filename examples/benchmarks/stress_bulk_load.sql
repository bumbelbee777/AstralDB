-- Off-CI bulk load + metadata-friendly scan probes.

DROP TABLE IF EXISTS stress_bulk;

CREATE TABLE stress_bulk (id INT, a INT, b TEXT, c TEXT, d TEXT);

BEGIN;
INSERT INTO stress_bulk BULK 150000 START 1 STEP 1;
COMMIT;

WITH slice AS (
	SELECT id, a, b
	FROM stress_bulk
	WHERE id BETWEEN 50000 AND 50100
)
SELECT id, a, b
FROM slice
ORDER BY id ASC
LIMIT 200;

WITH pref AS (
	SELECT a
	FROM stress_bulk
	WHERE b LIKE 'txt_1%'
)
SELECT DISTINCT a
FROM pref
ORDER BY a DESC
LIMIT 500;

DROP TABLE IF EXISTS stress_bulk;
