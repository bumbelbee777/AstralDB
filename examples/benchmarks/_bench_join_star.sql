-- 3-way star join + GROUP BY (spec-aligned; mirrors legacy _bench_heavy.sql).
SELECT
    c.country,
    p.category,
    COUNT(*) AS order_count,
    SUM(o.amount) AS total_amount
FROM customers c
JOIN orders o ON c.cust_id = o.cust_id
JOIN products p ON o.prod_id = p.prod_id
WHERE o.order_date >= '2024-01-01'
GROUP BY c.country, p.category
ORDER BY total_amount DESC
LIMIT 1000;
