-- WAL + DML churn: many commits, updates, deletes (isolated run recommended).
-- Lazy BULK with lazy DML overlays; final probe uses CTE for metadata filter/limit.

DROP TABLE IF EXISTS wal_churn;

CREATE TABLE wal_churn (id INT, a INT, b TEXT, c TEXT, d TEXT);

INSERT INTO wal_churn BULK 4096 START 1 STEP 1;
INSERT INTO wal_churn BULK 4096 START 4097 STEP 1;
INSERT INTO wal_churn BULK 4096 START 8193 STEP 1;

BEGIN;
UPDATE wal_churn SET c = 'w1' WHERE id BETWEEN 1 AND 6000;
UPDATE wal_churn SET c = 'w2' WHERE id BETWEEN 6001 AND 12288;
UPDATE wal_churn SET d = 'd1' WHERE a BETWEEN 100 AND 500;
COMMIT;

BEGIN;
DELETE FROM wal_churn WHERE b LIKE 'txt_19%' AND id < 2000;
DELETE FROM wal_churn WHERE id BETWEEN 11000 AND 12288;
COMMIT;

BEGIN;
UPDATE wal_churn SET b = 'repl' WHERE id BETWEEN 9000 AND 9500;
COMMIT;

WITH w2 AS (
	SELECT id, a, b, c, d
	FROM wal_churn
	WHERE c = 'w2'
)
SELECT id, a, b, c, d
FROM w2
ORDER BY id ASC
LIMIT 25;

DROP TABLE IF EXISTS wal_churn;
