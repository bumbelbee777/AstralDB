-- WAL + DML churn: many commits, updates, deletes (isolated run recommended).

DROP TABLE IF EXISTS wal_churn;

CREATE TABLE wal_churn (id INT, a INT, b TEXT, c TEXT, d TEXT);

INSERT INTO wal_churn BULK 35000 START 1 STEP 1;

BEGIN;
UPDATE wal_churn SET c = 'w1' WHERE id BETWEEN 1 AND 9000;
UPDATE wal_churn SET c = 'w2' WHERE id BETWEEN 9001 AND 18000;
UPDATE wal_churn SET d = 'd1' WHERE a BETWEEN 100 AND 500;
COMMIT;

BEGIN;
DELETE FROM wal_churn WHERE b LIKE 'txt_19%' AND id < 2000;
DELETE FROM wal_churn WHERE id BETWEEN 30000 AND 32000;
COMMIT;

BEGIN;
INSERT INTO wal_churn BULK 5000 START 40000 STEP 1;
UPDATE wal_churn SET b = 'repl' WHERE id BETWEEN 40000 AND 41500;
COMMIT;

SELECT * FROM wal_churn WHERE c = 'w2' ORDER BY id ASC LIMIT 25;

DROP TABLE IF EXISTS wal_churn;
