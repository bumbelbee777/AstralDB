-- Named datasets: register a table snapshot or bulk fixture, then LOAD INTO a target table.

CREATE TABLE src (id INT, a INT, b TEXT, c TEXT, d TEXT);
INSERT INTO src BULK 50 START 1 STEP 1;

CREATE DATASET snap AS TABLE src;
CREATE DATASET bench AS BULK 100 START 1000 STEP 1;

CREATE TABLE dst (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET snap INTO dst;

CREATE TABLE gen (id INT, a INT, b TEXT, c TEXT, d TEXT);
LOAD DATASET bench INTO gen;

SELECT id FROM dst ORDER BY id ASC LIMIT 5;
DROP DATASET snap;
DROP DATASET bench;

DROP TABLE IF EXISTS gen;
DROP TABLE IF EXISTS dst;
DROP TABLE IF EXISTS src;
