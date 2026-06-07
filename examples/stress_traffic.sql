-- Simulated production traffic: lazy bulk + lazy DML + metadata star join.
-- Run with: astraldb --time-sql -m examples/stress_traffic.sql

DROP TABLE IF EXISTS st_orders;
DROP TABLE IF EXISTS st_customers;

CREATE TABLE st_customers (id INT, a INT, b TEXT, c TEXT, d TEXT);
CREATE TABLE st_orders (id INT, a INT, b TEXT, c TEXT, d TEXT);

BEGIN;
INSERT INTO st_customers BULK 4096 START 1 STEP 1;
INSERT INTO st_orders BULK 4096 START 1 STEP 1;
INSERT INTO st_orders BULK 4096 START 4097 STEP 1;
COMMIT;

BEGIN;
UPDATE st_orders SET c = 'paid' WHERE id BETWEEN 50 AND 7500;
UPDATE st_customers SET d = 'vip' WHERE id BETWEEN 1 AND 2000;
UPDATE st_orders SET b = 'rush' WHERE c = 'paid' AND id BETWEEN 100 AND 6000;
COMMIT;

WITH j AS (
	SELECT st_customers.id, st_customers.a
	FROM st_customers
	INNER JOIN st_orders ON st_customers.id = st_orders.id
	GROUP BY st_customers.id, st_customers.a
)
SELECT id, a
FROM j
LIMIT 100;

BEGIN;
DELETE FROM st_orders WHERE id BETWEEN 1 AND 200;
INSERT INTO st_orders BULK 4096 START 8193 STEP 1;
COMMIT;

WITH tail AS (
	SELECT id
	FROM st_orders
)
SELECT id
FROM tail
ORDER BY id DESC
LIMIT 10;

WITH cust AS (
	SELECT a
	FROM st_customers
)
SELECT DISTINCT a
FROM cust
ORDER BY a DESC
LIMIT 30;

DROP TABLE IF EXISTS st_orders;
DROP TABLE IF EXISTS st_customers;
