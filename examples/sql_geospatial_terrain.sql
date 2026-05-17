-- Terrain / DEM: 3D points and bilinear elevation sampling.



CREATE TABLE sites (id INT, loc TERRAIN);

INSERT INTO sites (id, loc) VALUES (1, 'Z(-73.98,40.75,10.5)');

INSERT INTO sites (id, loc) VALUES (2, 'Z(0,0,100)');



SELECT id, ST_ELEVATION(loc) AS elev FROM sites ORDER BY id;



CREATE TABLE dem_grid (id INT, dem MATRIX(2,2));

INSERT INTO dem_grid (id, dem) VALUES (1, 'T[2,2]:0,10,20,30');



SELECT ST_DEM_SAMPLE(dem, 2, 2, 0, 0, 1, 1, 0.25, 0.25) AS sample_elev,

       ST_TERRAIN_SLOPE(dem, 2, 2, 0, 0, 1, 1, 0.5, 0.5) AS slope_deg

FROM dem_grid;



DROP TABLE IF EXISTS dem_grid;

DROP TABLE IF EXISTS sites;

