-- Timing harness: `astraldb --time-sql examples/join_bench.sql`
-- BulkInsert codegen currently emits torture-shaped 5-column rows (see BulkInsertAST in Codegen.cxx).
DROP TABLE IF EXISTS jb_a;
DROP TABLE IF EXISTS jb_b;
CREATE TABLE jb_a (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE jb_b (id INT, a INT, b TEXT, c TEXT, d TEXT);
INSERT INTO jb_a BULK 1500 START 1 STEP 1;
INSERT INTO jb_b BULK 1500 START 1 STEP 1;
SELECT id, cnt FROM jb_a INNER JOIN jb_b ON jb_a.id = jb_b.id GROUP BY id LIMIT 800;
