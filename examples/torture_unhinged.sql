-- Off-CI unhinged harness: bulk + WAL + geo/MathSci + datasets + maintenance spikes.
-- Run: astraldb -O4 --time-sql examples/torture_unhinged.sql
-- Lazy bulk + lazy DML + lazy dataset snapshot; metadata fast paths on star join.

DROP TABLE IF EXISTS tu_dst;
DROP TABLE IF EXISTS tu_geo;
DROP TABLE IF EXISTS tu_core;
DROP TABLE IF EXISTS tu_dml;
DROP TABLE IF EXISTS tu_lab;

CREATE TABLE tu_core (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE tu_geo (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE tu_dml (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE tu_lab (
	id INT,
	vals LIST(DOUBLE),
	pos POINT,
	ts TEXT
);

BEGIN;
INSERT INTO tu_core BULK 4096 START 1 STEP 1;
INSERT INTO tu_core BULK 4096 START 4097 STEP 1;
INSERT INTO tu_geo BULK 4096 START 1 STEP 1;
INSERT INTO tu_dml BULK 4096 START 1 STEP 1;
INSERT INTO tu_dml BULK 4096 START 4097 STEP 1;
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

SELECT
	ST_DISTANCE_SPHERICAL(pos, ST_POINT(-74, 41)) AS dist_m,
	ODE_HEUN('L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.11,0', '0.01') AS heun_y,
	TS_COMPRESS(vals) AS packed
FROM tu_lab;

WITH agg AS (
	SELECT tu_core.id, COUNT(tu_geo.id) AS cnt
	FROM tu_core
	INNER JOIN tu_geo ON tu_core.id = tu_geo.id
	GROUP BY tu_core.id
	HAVING COUNT(tu_geo.id) > 0
)
SELECT id, cnt
FROM agg
ORDER BY cnt DESC
LIMIT 50;

BEGIN;
UPDATE tu_dml SET c = 'hot' WHERE id BETWEEN 1500 AND 5000;
UPDATE tu_dml SET d = 'zap' WHERE a BETWEEN 1 AND 40 OR id = 4242;
DELETE FROM tu_dml WHERE id BETWEEN 7500 AND 8000;
UPDATE tu_dml SET b = 'repl' WHERE id BETWEEN 5000 AND 5500;
COMMIT;

CREATE DATASET tu_snap AS TABLE tu_dml;
CREATE TABLE tu_dst (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET tu_snap INTO tu_dst;

WITH hot AS (
	SELECT a
	FROM tu_dml
	WHERE c = 'hot'
)
SELECT DISTINCT a
FROM hot
ORDER BY a DESC
LIMIT 80;

SELECT id, a FROM tu_dst ORDER BY id DESC LIMIT 30;

VACUUM TABLE tu_dml;

DROP DATASET tu_snap;
DROP TABLE IF EXISTS tu_dst;
DROP TABLE IF EXISTS tu_lab;
DROP TABLE IF EXISTS tu_geo;
DROP TABLE IF EXISTS tu_core;
DROP TABLE IF EXISTS tu_dml;
