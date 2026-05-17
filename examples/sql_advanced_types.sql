-- DuckDB-style advanced types: STRUCT, MAP, VECTOR, MATRIX, COMPLEX (SIMD-backed ops).

CREATE TABLE samples (
	id INT,
	pos STRUCT(x FLOAT, y FLOAT),
	tags MAP(TEXT, TEXT),
	ori COMPLEX,
	vel VECTOR(3) FLOAT,
	basis MATRIX(2, 2) FLOAT
);

INSERT INTO samples VALUES (
	1,
	'S{x=1,y=2}',
	'M{role:admin,team:core}',
	'C(1,2)',
	'V[3]:1,0,0',
	'T[2,2]:1,0,0,1'
);

SELECT
	id,
	STRUCT_FIELD(pos, 'x') AS x,
	MAP_GET(tags, 'role') AS role,
	COMPLEX_REAL(ori) AS re,
	VECTOR_NORM(vel) AS speed,
	VECTOR_DOT(vel, 'V[3]:1,0,0') AS forward
FROM samples;

SELECT MATRIX_VEC(basis, vel) AS transformed FROM samples;
