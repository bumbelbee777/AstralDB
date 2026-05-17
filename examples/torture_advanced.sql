-- Advanced torture: types, geo, MathSci, time-series compression, datasets, hybrid patterns.

DROP TABLE IF EXISTS ta_geo;
DROP TABLE IF EXISTS ta_lab;
DROP TABLE IF EXISTS ta_dst;

CREATE TABLE ta_lab (
	id INT,
	vals LIST(DOUBLE),
	pos POINT,
	ts TEXT
);

INSERT INTO ta_lab (id, vals, pos, ts) VALUES (
	1,
	'L[4]:1,2,3,4',
	'G(-73.98,40.75)',
	'2024-06-01T12:00:00'
);

CREATE TABLE ta_geo (id INT, a INT, b TEXT, c TEXT, d TEXT);
INSERT INTO ta_geo BULK 1200 START 1 STEP 1;

INSERT INTO ta_lab (id, vals, pos, ts) VALUES (
	3,
	'L[6]:0.5,1.5,2.5,3.5,4.5,5.5',
	'G(2.35,48.86)',
	'2024-07-15T18:45:00'
);

SELECT
	ST_DISTANCE_SPHERICAL(pos, ST_POINT(-74, 41)) AS dist_m,
	ODE_HEUN('L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.11,0', '0.01') AS heun_y,
	TS_COMPRESS(vals) AS packed
FROM ta_lab;

CREATE DATASET ta_snap AS TABLE ta_geo;
CREATE TABLE ta_dst (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET ta_snap INTO ta_dst;

SELECT id, a FROM ta_dst ORDER BY id DESC LIMIT 20;

UPDATE ta_geo SET c = 'mut' WHERE id BETWEEN 100 AND 900;
VACUUM TABLE ta_geo;

DROP DATASET ta_snap;
DROP TABLE IF EXISTS ta_dst;
DROP TABLE IF EXISTS ta_geo;
DROP TABLE IF EXISTS ta_lab;
