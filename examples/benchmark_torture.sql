-- CI-scale analytic + join workload (AstralDB-native; no generate_series).
-- For million-row cross-engine benches use examples/benchmarks/ and scripts/benchmark_torture_plot.py

DROP TABLE IF EXISTS bt_cust;
DROP TABLE IF EXISTS bt_ord;
DROP TABLE IF EXISTS bt_line;

CREATE TABLE bt_cust (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE bt_ord (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE bt_line (id INT, a INT, b TEXT, c TEXT, d TEXT);

INSERT INTO bt_cust BULK 800 START 1 STEP 1;
INSERT INTO bt_ord BULK 3200 START 1 STEP 1;
INSERT INTO bt_line BULK 12000 START 1 STEP 1;

SELECT bt_cust.id, COUNT(bt_ord.id) AS order_cnt
FROM bt_cust
INNER JOIN bt_ord ON bt_cust.id = bt_ord.id
GROUP BY bt_cust.id
HAVING COUNT(bt_ord.id) > 0
ORDER BY order_cnt DESC
LIMIT 50;

SELECT bt_line.id, bt_line.a
FROM bt_line
INNER JOIN bt_ord ON bt_line.id = bt_ord.id
WHERE bt_line.a BETWEEN 10 AND 900
ORDER BY bt_line.a DESC
LIMIT 200;

DROP TABLE IF EXISTS bt_line;
DROP TABLE IF EXISTS bt_ord;
DROP TABLE IF EXISTS bt_cust;
