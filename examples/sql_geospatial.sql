-- Geospatial: POINT type and ST_* scalars (WGS84 lon/lat in degrees).

CREATE TABLE sites (
	id INT,
	name TEXT,
	loc POINT
);

INSERT INTO sites (id, name, loc) VALUES (1, 'origin', 'G(0,0)');
INSERT INTO sites (id, name, loc) VALUES (2, 'near', 'G(0.01,0.01)');

SELECT
	name,
	ST_X(loc) AS lon,
	ST_Y(loc) AS lat,
	ST_AS_TEXT(loc) AS wkt
FROM sites
ORDER BY id ASC;

SELECT ST_DISTANCE(loc, 'G(0,0)') AS planar,
       ST_DISTANCE_SPHERICAL(loc, 'G(0,0)') AS meters,
       ST_WITHIN_BBOX(loc, -1, -1, 1, 1) AS in_box
FROM sites
ORDER BY id ASC;

SELECT ST_POINT(-73.98, 40.75) AS nyc, ST_DISTANCE_SPHERICAL('G(-73.98,40.75)', 'G(0,0.01)') AS d;
