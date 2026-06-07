-- antimatterbomb Query 1 shape: CUBE + join (run after antimatterbomb_setup.sql).

WITH order_analytics AS (
    SELECT
        c.cust_id,
        c.country,
        p.category,
        DATE_TRUNC('month', o.order_date) AS month,
        COUNT(*) AS order_count,
        SUM(o.amount) AS total_amount,
        AVG(o.amount) AS avg_amount
    FROM customers c
    JOIN orders o ON c.cust_id = o.cust_id
    JOIN products p ON o.prod_id = p.prod_id
    WHERE o.order_date >= '2024-01-01'
    GROUP BY CUBE(c.cust_id, c.country, p.category, month)
    HAVING COUNT(*) > 10
)
SELECT
    country,
    category,
    month,
    AVG(total_amount) AS avg_total,
    SUM(order_count) AS total_orders
FROM order_analytics
WHERE month IS NOT NULL
GROUP BY country, category, month
ORDER BY country, month
LIMIT 1000;
