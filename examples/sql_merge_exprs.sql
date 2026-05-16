-- Expression SET and composite MERGE ON
CREATE TABLE mt (id INT PRIMARY KEY, a INT, b INT);
CREATE TABLE ms (sk INT, x INT, y INT);
INSERT INTO mt VALUES (1, 10, 1);
INSERT INTO ms VALUES (1, 5, 2);

MERGE INTO mt AS t USING ms AS s ON t.id = s.sk AND t.b = s.y
WHEN MATCHED THEN UPDATE SET a = t.a + s.x
WHEN NOT MATCHED THEN INSERT (id, a, b) VALUES (s.sk, s.x, s.y);

UPDATE mt SET b = b + 1 WHERE id = 1;
