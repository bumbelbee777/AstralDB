-- Window frames: default running SUM and explicit ROWS BETWEEN (see docs/Overview.md).

CREATE TABLE sales (dept TEXT, amt INT);
INSERT INTO sales VALUES ('A', 5), ('A', 15), ('B', 100);
SELECT dept, amt, SUM(amt) OVER (PARTITION BY dept ORDER BY amt ASC) AS run_sum FROM sales;

CREATE TABLE ticks (p INT);
INSERT INTO ticks VALUES (1), (2), (3), (4), (5);
SELECT p, SUM(p) OVER (ORDER BY p ASC ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS w3 FROM ticks;
