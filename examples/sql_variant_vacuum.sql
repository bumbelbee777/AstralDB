-- VARIANT column type, VACUUM, and REPACK TABLE CONCURRENTLY.



CREATE TABLE mixed (id INT, payload VARIANT);

INSERT INTO mixed (id, payload) VALUES (1, 'V{i:42}');

INSERT INTO mixed (id, payload) VALUES (2, 'V{s:hello}');

INSERT INTO mixed (id, payload) VALUES (3, 'V{r:3.14}');



SELECT id, payload FROM mixed ORDER BY id;



VACUUM TABLE mixed;

REPACK TABLE mixed CONCURRENTLY;



SELECT id, payload FROM mixed ORDER BY id;



DROP TABLE IF EXISTS mixed;

