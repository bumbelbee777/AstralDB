-- AstralDB durable SQL-92 bench: lazy BULK star schema (id-aligned joins for metadata).
-- Row counts rewritten by scripts/benchmark_torture_plot.py from --scale.

DROP TABLE IF EXISTS bt_line;
DROP TABLE IF EXISTS bt_ord;
DROP TABLE IF EXISTS bt_cust;

CREATE TABLE bt_cust (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE bt_ord (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE bt_line (id INT, a INT, b TEXT, c TEXT, d TEXT);

INSERT INTO bt_cust BULK 1000 START 1 STEP 1;
INSERT INTO bt_ord BULK 10000 START 1 STEP 1;
INSERT INTO bt_line BULK 50000 START 1 STEP 1;
