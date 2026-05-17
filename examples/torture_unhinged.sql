-- Off-CI unhinged harness: bulk + WAL + geo/MathSci + datasets + maintenance spikes.
-- Run: astraldb -O2 --time-sql examples/torture_unhinged.sql
-- Plot suite: scripts/stress_torture_histogram.py

DROP TABLE IF EXISTS tu_dst;
DROP TABLE IF EXISTS tu_geo;
DROP TABLE IF EXISTS tu_core;
DROP TABLE IF EXISTS tu_lab;

CREATE TABLE tu_core (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE tu_geo (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE tu_lab (
	id INT,
	vals LIST(DOUBLE),
	pos POINT,
	ts TEXT
);

BEGIN;
INSERT INTO tu_core BULK 12000 START 1 STEP 1;
INSERT INTO tu_geo BULK 4000 START 1 STEP 1;
INSERT INTO tu_lab (id, vals, pos, ts) VALUES (
	1,
	'L[8]:1,2,3,4,5,6,7,8',
	'G(-73.98,40.75)',
	'2024-06-01T12:00:00'
);
INSERT INTO tu_lab (id, vals, pos, ts) VALUES (
	2,
	'L[4]:9,8,7,6',
	'G(-118.24,34.05)',
	'2024-06-02T08:30:00'
);
COMMIT;

BEGIN;
UPDATE tu_core SET c = 'hot' WHERE id > 9000 AND id < 11500;
UPDATE tu_core SET d = 'zap' WHERE a BETWEEN 1 AND 40 OR id = 4242;
UPDATE tu_geo SET c = 'geo_hot' WHERE id BETWEEN 500 AND 3500;
DELETE FROM tu_core WHERE b LIKE 'txt_9%' AND id BETWEEN 900 AND 1200;
COMMIT;

SELECT
	ST_DISTANCE_SPHERICAL(pos, ST_POINT(-74, 41)) AS dist_m,
	ODE_HEUN('L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.11,0', '0.01') AS heun_y,
	TS_COMPRESS(vals) AS packed
FROM tu_lab;

SELECT tu_core.id, COUNT(tu_geo.id) AS cnt
FROM tu_core
INNER JOIN tu_geo ON tu_core.id = tu_geo.id
GROUP BY tu_core.id
HAVING COUNT(tu_geo.id) > 0
ORDER BY cnt DESC
LIMIT 80;

BEGIN;
DELETE FROM tu_core WHERE id BETWEEN 11000 AND 12000;
INSERT INTO tu_core BULK 2500 START 20000 STEP 1;
UPDATE tu_core SET b = 'repl' WHERE id BETWEEN 20000 AND 21000;
COMMIT;

CREATE DATASET tu_snap AS TABLE tu_core;
CREATE TABLE tu_dst (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET tu_snap INTO tu_dst;

SELECT id, a FROM tu_dst ORDER BY id DESC LIMIT 40;
SELECT DISTINCT a FROM tu_core WHERE c = 'hot' ORDER BY a DESC LIMIT 120;

VACUUM TABLE tu_core;
REPACK TABLE tu_geo CONCURRENTLY;

DROP DATASET tu_snap;
DROP TABLE IF EXISTS tu_dst;
DROP TABLE IF EXISTS tu_lab;
DROP TABLE IF EXISTS tu_geo;
DROP TABLE IF EXISTS tu_core;
