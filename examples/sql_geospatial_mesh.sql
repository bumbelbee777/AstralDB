-- Geospatial MESH: definition, glTF import/export, sewing, simplified CSG.

CREATE TABLE meshes (
	id INT,
	shape MESH
);

INSERT INTO meshes (id, shape)
VALUES (1, ST_MESH('V[9]:0,0,0,1,0,0,0,1,0', 'L[3]:0,1,2'));

INSERT INTO meshes (id, shape)
VALUES (2, ST_MESH_IMPORT_GLTF('{"asset":{"version":"2.0"},"meshes":[{"primitives":[{"POSITION":[[0,0,0],[1,0,0],[0,1,0]],"indices":[0,1,2]}]}]}'));

SELECT ST_MESH_EXPORT_GLTF(shape) AS gltf_json
FROM meshes
WHERE id = 1;

SELECT
	ST_MESH_SEW(shape, 0.0001) AS sewn,
	ST_MESH_UNION(shape, shape) AS u,
	ST_MESH_INTERSECTION(shape, shape) AS i,
	ST_MESH_DIFFERENCE(shape, shape) AS d,
	ST_MESH_SURFACE_AREA(shape) AS area,
	ST_MESH_VOLUME(shape) AS volume,
	ST_MESH_CENTROID(shape) AS centroid,
	ST_MESH_BOUNDS(shape) AS bounds
FROM meshes
WHERE id = 1;
