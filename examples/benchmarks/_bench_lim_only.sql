-- LIMIT 1000 on indexed primary key (no WHERE).
SELECT * FROM orders ORDER BY order_id LIMIT 1000;
