-- Profile nuke.sql hot path: `astraldb -m -O4 --time-sql examples/nuke_profile.sql`
PRAGMA profile('nuke');
PRAGMA region('setup');
CREATE TABLE txns (id INT PRIMARY KEY, acct INT, amount DECIMAL, ts TIMESTAMP);
INSERT INTO txns BULK 10000000 START 1 STEP 1;
PRAGMA region('query');
SELECT acct, SUM(amount) OVER (PARTITION BY acct ORDER BY ts ROWS BETWEEN 5 PRECEDING AND CURRENT ROW)
FROM txns
WHERE ts >= '2024-01-01';
PRAGMA profile_dump('nuke_profile.json');
