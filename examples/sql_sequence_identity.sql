-- Sequences and GENERATED AS IDENTITY (see docs/Overview.md).

CREATE SEQUENCE order_seq START WITH 100 INCREMENT BY 1;

CREATE TABLE orders (
	id INT GENERATED ALWAYS AS IDENTITY (START WITH 1 INCREMENT BY 1),
	ref INT PRIMARY KEY
);

INSERT INTO orders (ref) VALUES (1);
INSERT INTO orders (ref) VALUES (2);

CREATE TABLE tickets (tid INT PRIMARY KEY, note TEXT);
INSERT INTO tickets VALUES (NEXTVAL(order_seq), 'from-seq');

SELECT ref FROM orders ORDER BY ref ASC;
SELECT tid, note FROM tickets ORDER BY tid ASC;
