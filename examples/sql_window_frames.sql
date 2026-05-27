-- Window frames: default running SUM and explicit ROWS BETWEEN (see docs/Overview.md).

CREATE TABLE sales (dept TEXT, amt INT);
INSERT INTO sales VALUES ('A', 5), ('A', 15), ('B', 100);
SELECT dept, amt, SUM(amt) OVER (PARTITION BY dept ORDER BY amt ASC) AS run_sum FROM sales;

CREATE TABLE ticks (p INT);
INSERT INTO ticks VALUES (1), (2), (3), (4), (5);
SELECT p, SUM(p) OVER (ORDER BY p ASC ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS w3 FROM ticks;

CREATE TABLE vals (x INT);
INSERT INTO vals VALUES (1), (3), (5), (7), (9);
SELECT x, SUM(x) OVER (ORDER BY x ASC RANGE BETWEEN 2 PRECEDING AND CURRENT ROW) AS rsum FROM vals;
DROP TABLE vals;
