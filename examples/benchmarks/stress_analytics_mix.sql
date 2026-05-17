-- Heavy joins, GROUP BY, ORDER BY at medium scale (not run in examples/*.sql CI).

DROP TABLE IF EXISTS sa_dim;
DROP TABLE IF EXISTS sa_fact;

CREATE TABLE sa_dim (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE sa_fact (id INT, a INT, b TEXT, c TEXT, d TEXT);

INSERT INTO sa_dim BULK 8000 START 1 STEP 1;
INSERT INTO sa_fact BULK 32000 START 1 STEP 1;

SELECT sa_dim.id, COUNT(sa_fact.id) AS cnt, MAX(sa_fact.a) AS peak
FROM sa_dim
INNER JOIN sa_fact ON sa_dim.id = sa_fact.id
GROUP BY sa_dim.id
HAVING COUNT(sa_fact.id) > 1
ORDER BY cnt DESC
LIMIT 100;

SELECT sa_fact.id, sa_fact.b
FROM sa_fact
WHERE a BETWEEN 10 AND 500
ORDER BY sa_fact.id DESC
LIMIT 300;

DROP TABLE IF EXISTS sa_fact;
DROP TABLE IF EXISTS sa_dim;
