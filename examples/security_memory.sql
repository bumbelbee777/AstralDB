-- Security contract: parser limits and maintenance commands (run under examples harness).

CREATE TABLE t (id INT, x TEXT);
INSERT INTO t (id, x) VALUES (1, 'ok');
VACUUM;
REPACK TABLE t CONCURRENTLY;
SELECT id FROM t;

DROP TABLE IF EXISTS t;
