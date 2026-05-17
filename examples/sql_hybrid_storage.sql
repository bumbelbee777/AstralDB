-- Hybrid storage: row store (OLTP default), columnar (analytics), MLP AUTO tuning.

CREATE TABLE sensor_data (id INT, region TEXT, temperature INT)
USING STORAGE AUTO;

INSERT INTO sensor_data VALUES (1, 'east', 22), (2, 'west', 18), (3, 'east', 25);

-- Analytics-friendly scan (optional per-query hint).
SELECT /*+ STORAGE(COLUMNAR) */ region, AVG(temperature) AS avg_temp
FROM sensor_data
GROUP BY region;

-- Pin layout explicitly.
ALTER TABLE sensor_data SET STORAGE COLUMNAR;

SELECT region, SUM(temperature) AS total FROM sensor_data GROUP BY region;
