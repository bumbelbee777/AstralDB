-- Same workload as nuke.sql with LIMIT (logical 10M-row result; columnar projection, no row maps).
-- Benchmark harness: on-disk --database + --time-sql-durable (see scripts/benchmark_nuke_plot.py).
-- Run: astraldb -O4 --database nuke.db --time-sql-setup examples/nuke_materialize_setup.sql --time-sql examples/nuke_materialize_query.sql --time-sql-durable
CREATE TABLE txns (id INT PRIMARY KEY, acct INT, amount DECIMAL, ts TIMESTAMP);
INSERT INTO txns BULK 10000000 START 1 STEP 1;

SELECT acct,
  SUM(amount) OVER (PARTITION BY acct ORDER BY ts ROWS BETWEEN 5 PRECEDING AND CURRENT ROW) AS sum_amount
FROM txns
WHERE ts >= '2024-01-01'
LIMIT 10000000;
