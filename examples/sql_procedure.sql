-- Stored procedures: SQL syntax caches compiled .abc under astraldb_procs_cache/
CREATE PROCEDURE seed_data AS (
	CREATE TABLE proc_demo (id INTEGER, phrase TEXT);
	INSERT INTO proc_demo VALUES (1, 'from procedure');
);
CALL seed_data;
SELECT phrase FROM proc_demo;
