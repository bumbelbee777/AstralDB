-- antimatterbomb bench — phase 2: join + aggregate + multi-window (post bulk load).
-- Timed after setup: astraldb -m -O4 --time-sql examples/benchmarks/antimatterbomb_query.sql

SELECT
    c.country,
    p.category,
    COUNT(*) AS order_count,
    SUM(o.amount) AS total_amount,
    AVG(o.amount) AS avg_amount,
    MAX(o.quantity) AS peak_qty
FROM customers c
JOIN orders o ON c.cust_id = o.cust_id
JOIN products p ON o.prod_id = p.prod_id
WHERE o.order_date >= '2024-01-01'
GROUP BY c.country, p.category
ORDER BY total_amount DESC
LIMIT 1000;

SELECT
    order_id,
    cust_id,
    order_date,
    amount,
    SUM(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS UNBOUNDED PRECEDING) AS running_total,
    AVG(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS BETWEEN 30 PRECEDING AND CURRENT ROW) AS ma30,
    AVG(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS BETWEEN 7 PRECEDING AND CURRENT ROW) AS ma7,
    amount - LAG(amount, 1) OVER (PARTITION BY cust_id ORDER BY order_date) AS dod_change,
    RANK() OVER (PARTITION BY DATE_TRUNC('month', order_date) ORDER BY amount DESC) AS monthly_rank
FROM orders
WHERE order_date >= '2024-01-01'
ORDER BY cust_id, order_date
LIMIT 100000;
