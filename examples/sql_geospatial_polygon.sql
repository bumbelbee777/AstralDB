-- 2D POLYGON geospatial: metrics, predicates, booleans, buffer, simplify, GeoJSON.

CREATE TABLE parcels (
	id INT,
	geom POLYGON
);

INSERT INTO parcels (id, geom) VALUES (
	1,
	ST_POLYGON('V[10]:0,0,2,0,2,2,0,2,0,0')
);

INSERT INTO parcels (id, geom) VALUES (
	2,
	ST_POLYGON_WKT('POLYGON((1 0,3 0,3 2,1 2,1 0))')
);

SELECT
	id,
	ST_GEOM_AREA(geom) AS area,
	ST_GEOM_PERIMETER(geom) AS perimeter,
	ST_GEOM_AS_TEXT(geom) AS wkt
FROM parcels
ORDER BY id;

SELECT
	ST_GEOM_CONTAINS(
		(SELECT geom FROM parcels WHERE id = 1),
		'G(1,1)'
	) AS contains_pt,
	ST_GEOM_INTERSECTS(
		(SELECT geom FROM parcels WHERE id = 1),
		(SELECT geom FROM parcels WHERE id = 2)
	) AS intersects
FROM parcels
WHERE id = 1;

SELECT
	ST_GEOM_UNION(
		(SELECT geom FROM parcels WHERE id = 1),
		(SELECT geom FROM parcels WHERE id = 2)
	) AS u,
	ST_GEOM_INTERSECTION(
		(SELECT geom FROM parcels WHERE id = 1),
		(SELECT geom FROM parcels WHERE id = 2)
	) AS i,
	ST_GEOM_BUFFER((SELECT geom FROM parcels WHERE id = 1), 0.1) AS buffered
FROM parcels
WHERE id = 1;

SELECT ST_GEOJSON_EXPORT(geom) AS geojson FROM parcels WHERE id = 1;
