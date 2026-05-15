-- Subset exercised by unit tests (GROUP BY, COUNT(*), EXISTS, WITH, ROW_NUMBER).

CREATE TABLE g (k INT);
INSERT INTO g VALUES (1), (1), (2);
SELECT k, cnt FROM g GROUP BY k;

CREATE TABLE a (id INT);
CREATE TABLE b (ref INT);
INSERT INTO a VALUES (1),(2);
INSERT INTO b VALUES (1);
SELECT id FROM a WHERE EXISTS (SELECT * FROM b WHERE ref = 1);

CREATE TABLE raw (id INT);
INSERT INTO raw VALUES (100), (200);
WITH wrap AS (SELECT id FROM raw WHERE id >= 150) SELECT id FROM wrap ORDER BY id ASC;

CREATE TABLE ord (n INT);
INSERT INTO ord VALUES (3), (1), (4);
SELECT ROW_NUMBER() OVER (ORDER BY n ASC) AS r FROM ord;

CREATE TABLE winp (g TEXT, n INT);
INSERT INTO winp VALUES ('a', 2), ('a', 1), ('b', 1);
SELECT ROW_NUMBER() OVER (PARTITION BY g ORDER BY n ASC) AS r FROM winp;

CREATE TABLE u (id INT);
CREATE TABLE v (id INT, tag TEXT);
INSERT INTO u VALUES (1),(2);
INSERT INTO v VALUES (1,'x'),(2,'y');
SELECT u.id, tag FROM u INNER JOIN v ON u.id = v.id;

CREATE TABLE sales (region TEXT, amt INT);
INSERT INTO sales VALUES ('east','10'), ('east','20'), ('west','5');
SELECT region, SUM ( amt ) AS total FROM sales GROUP BY region;
