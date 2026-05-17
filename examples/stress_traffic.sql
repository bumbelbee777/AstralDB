-- Simulated production traffic: many short transactions, mixed DML, joins, aggregates.
-- Run with: astraldb --time-sql -m examples/stress_traffic.sql

DROP TABLE IF EXISTS st_orders;
DROP TABLE IF EXISTS st_customers;

CREATE TABLE st_customers (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE st_orders (id INT, a INT, b TEXT, c TEXT, d TEXT);

BEGIN;
INSERT INTO st_customers BULK 1200 START 1 STEP 1;
INSERT INTO st_orders BULK 4800 START 1 STEP 1;
COMMIT;

BEGIN;
UPDATE st_orders SET c = 'paid' WHERE id BETWEEN 100 AND 4000 AND a BETWEEN 0 AND 996;
UPDATE st_customers SET d = 'vip' WHERE id BETWEEN 1 AND 200;
COMMIT;

SELECT st_customers.id, st_customers.a
FROM st_customers
INNER JOIN st_orders ON st_customers.id = st_orders.id
GROUP BY st_customers.id, st_customers.a
LIMIT 500;

BEGIN;
DELETE FROM st_orders WHERE id BETWEEN 1 AND 499;
INSERT INTO st_orders BULK 400 START 5000 STEP 1;
COMMIT;

SELECT id FROM st_orders ORDER BY id DESC LIMIT 10;
SELECT DISTINCT a FROM st_customers ORDER BY a DESC LIMIT 100;

DROP TABLE IF EXISTS st_orders;
DROP TABLE IF EXISTS st_customers;
