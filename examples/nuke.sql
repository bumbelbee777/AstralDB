-- 10M-row bench: run with `astraldb -m -O4 --time-sql examples/nuke.sql`
CREATE TABLE txns (id INT PRIMARY KEY, acct INT, amount DECIMAL, ts TIMESTAMP);
INSERT INTO txns BULK 10000000 START 1 STEP 1;

SELECT acct, SUM(amount) OVER (PARTITION BY acct ORDER BY ts ROWS BETWEEN 5 PRECEDING AND CURRENT ROW)
FROM txns
WHERE ts >= '2024-01-01';
