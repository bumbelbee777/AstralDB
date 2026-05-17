-- Parse + BULK insert + ORDER BY + UPDATE (timing: astraldb --time-sql examples/performance_test.sql)

DROP TABLE IF EXISTS perf_row;
CREATE TABLE perf_row (id INT, a INT, b TEXT, c TEXT, d TEXT);

INSERT INTO perf_row BULK 800 START 1 STEP 1;

SELECT id, a, b, c FROM perf_row ORDER BY id ASC LIMIT 100;
UPDATE perf_row SET b = 'hot' WHERE id BETWEEN 700 AND 799;
SELECT id, b FROM perf_row WHERE b = 'hot' ORDER BY id ASC;

DROP TABLE IF EXISTS perf_row;
