-- 10M rows, 5 tables, heavy JOINs, aggregations
-- Test to benchmark against DuckDB and SQLite

WITH 
customers AS (SELECT * FROM generate_series(1,1000000) id),
orders AS (SELECT * FROM generate_series(1,10000000) id),
line_items AS (SELECT * FROM generate_series(1,50000000) id)

SELECT 
    c.id,
    COUNT(o.id) as order_count,
    SUM(li.amount) as total_spent,
    AVG(li.amount) as avg_order,
    MIN(li.amount) as min_order,
    MAX(li.amount) as max_order
FROM customers c
INNER JOIN orders o ON o.customer_id = c.id
INNER JOIN line_items li ON li.order_id = o.id
WHERE c.created_at > '2026-01-01'
GROUP BY c.id, c.name
HAVING COUNT(o.id) > 5
ORDER BY total_spent DESC
LIMIT 100;