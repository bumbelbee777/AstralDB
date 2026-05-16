-- Rudimentary OLAP: GROUP BY … WITH ROLLUP / WITH CUBE (subtotals + grand total).
-- Rolled-up dimension columns are empty strings; _olap_level tags the grouping set
-- (ROLLUP: 0 = grand total, higher = more detail keys; CUBE: bitmask of active keys).

CREATE TABLE sales (dept TEXT, region TEXT, amt INT);
INSERT INTO sales VALUES ('eng', 'east', 10), ('eng', 'west', 20), ('mkt', 'east', 5);

SELECT dept, region, SUM(amt) AS total
FROM sales
GROUP BY dept, region
WITH ROLLUP;

-- Cube over a single key (four grouping sets for one column).
CREATE TABLE cube1 (k TEXT, v INT);
INSERT INTO cube1 VALUES ('x', 1), ('y', 2);
SELECT k, SUM(v) AS s FROM cube1 GROUP BY k WITH CUBE;
