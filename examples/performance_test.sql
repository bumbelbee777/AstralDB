-- Small batch: parse + INSERT + ORDER BY + UPDATE.

CREATE TABLE perf_row (id INTEGER, val1 INTEGER, val2 INTEGER, label VARCHAR);
INSERT INTO perf_row VALUES (1, 10, 20, 'a');
INSERT INTO perf_row VALUES (2, 11, 21, 'b');
INSERT INTO perf_row VALUES (3, 12, 22, 'c');

SELECT id, val1, val2, label FROM perf_row ORDER BY id ASC;
UPDATE perf_row SET val2 = 99 WHERE id = 3;
