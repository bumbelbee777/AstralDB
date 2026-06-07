-- Omnibus torture: modern SQL + MathSci + GQL + 2D/3D geo + bulk DML (perf tier).
-- Run: astraldb -O4 --time-sql examples/torture_omnibus.sql
-- Read-only star join uses lazy BULK (>= 4096); DML/dataset table uses materialized chunks (< 4096).

DROP TABLE IF EXISTS om_audit;
DROP TABLE IF EXISTS om_orders;
DROP TABLE IF EXISTS om_ranks;
DROP TABLE IF EXISTS om_paths;
DROP TABLE IF EXISTS om_follows;
DROP TABLE IF EXISTS om_users;
DROP TABLE IF EXISTS om_dst;
DROP TABLE IF EXISTS om_lab;
DROP TABLE IF EXISTS om_geo;
DROP TABLE IF EXISTS om_core;
DROP TABLE IF EXISTS om_dml;

CREATE TABLE om_users (id INT, name TEXT);
CREATE TABLE om_follows (src INT, dst INT, kind TEXT);
INSERT INTO om_users VALUES (1, 'A');
INSERT INTO om_users VALUES (2, 'B');
INSERT INTO om_users VALUES (3, 'C');
INSERT INTO om_users VALUES (4, 'D');
INSERT INTO om_follows VALUES (1, 2, 'follows');
INSERT INTO om_follows VALUES (2, 3, 'follows');
INSERT INTO om_follows VALUES (3, 4, 'follows');
INSERT INTO om_follows VALUES (4, 1, 'follows');

CREATE GRAPH om_social
	VERTEX TABLE om_users (id)
	EDGE TABLE om_follows (src, dst, kind);

CREATE TABLE om_core (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE om_geo (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE om_lab (id INT, vals LIST(DOUBLE), pos POINT);

INSERT INTO om_core BULK 4096 START 1 STEP 1;
INSERT INTO om_geo BULK 4096 START 1 STEP 1;

INSERT INTO om_lab (id, vals, pos) VALUES (
	1,
	'L[8]:1,2,3,4,5,6,7,8',
	'G(-73.98,40.75)'
);

SELECT
	ST_DISTANCE_SPHERICAL(pos, ST_POINT(-74, 41)) AS dist_m,
	FFT(vals) AS spectrum,
	ODE_HEUN('L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.11,0', '0.01') AS heun_y,
	NLP_TOKENIZE('omnibus graph geo mathsci bulk') AS nlp_toks
FROM om_lab
WHERE id = 1;

SELECT ST_GEOM_AREA(ST_POLYGON('V[10]:0,0,2,0,2,2,0,2,0,0')) AS parcel_area;
SELECT ST_MESH_VOLUME(ST_MESH('V[9]:0,0,0,2,0,0,0,2,0', 'L[3]:0,1,2')) AS mesh_vol;

GRAPH MATCH (a)-[e*1 .. 2]->(b) FROM 1 IN om_social INTO om_paths;
GRAPH PAGERANK IN om_social DAMPING 0.85 ITERATIONS 10 INTO om_ranks;

SELECT end_id, path_length FROM om_paths ORDER BY path_length, end_id LIMIT 20;
SELECT vertex_id, rank FROM om_ranks ORDER BY rank DESC LIMIT 10;

CREATE PROCEDURE om_seed AS (
	CREATE TABLE om_orders (id INTEGER, amount INTEGER);
	CREATE TABLE om_audit (id INTEGER, note TEXT);
);
CALL om_seed;

CREATE TRIGGER om_orders_ai
	AFTER INSERT ON om_orders
	FOR EACH ROW
	AS (
		INSERT INTO om_audit VALUES (1, 'fired');
	);

INSERT INTO om_orders VALUES (1, 50);

WITH agg AS (
	SELECT
		om_core.a AS bucket,
		COUNT(*) AS cnt
	FROM om_core
	INNER JOIN om_geo ON om_core.id = om_geo.id
	WHERE om_core.id BETWEEN 100 AND 4000
	GROUP BY om_core.a
	HAVING COUNT(*) > 0
)
SELECT
	bucket,
	cnt,
	RANK() OVER (ORDER BY cnt DESC) AS rk
FROM agg
ORDER BY rk
LIMIT 50;

CREATE TABLE om_dml (id INT, a INT, b TEXT, c TEXT, d TEXT);
INSERT INTO om_dml BULK 2000 START 1 STEP 1;
INSERT INTO om_dml BULK 2000 START 2001 STEP 1;

BEGIN;
UPDATE om_dml SET c = 'omni' WHERE id BETWEEN 1500 AND 2800;
UPDATE om_dml SET d = 'geo' WHERE id BETWEEN 500 AND 1800;
DELETE FROM om_dml WHERE id BETWEEN 3800 AND 4000;
INSERT INTO om_dml BULK 600 START 5000 STEP 1;
COMMIT;

CREATE DATASET om_snap AS TABLE om_dml;
CREATE TABLE om_dst (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET om_snap INTO om_dst;

SELECT id, a FROM om_dst ORDER BY id DESC LIMIT 10;

VACUUM TABLE om_dml;

DROP DATASET om_snap;
DROP TABLE IF EXISTS om_dst;
DROP TRIGGER IF EXISTS om_orders_ai;
DROP PROCEDURE IF EXISTS om_seed;
DROP TABLE IF EXISTS om_audit;
DROP TABLE IF EXISTS om_orders;
DROP TABLE IF EXISTS om_ranks;
DROP TABLE IF EXISTS om_paths;
DROP TABLE IF EXISTS om_follows;
DROP TABLE IF EXISTS om_users;
DROP TABLE IF EXISTS om_lab;
DROP TABLE IF EXISTS om_geo;
DROP TABLE IF EXISTS om_core;
DROP TABLE IF EXISTS om_dml;
