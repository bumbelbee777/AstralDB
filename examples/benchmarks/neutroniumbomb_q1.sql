-- Query 1: Star join + CUBE + windows + suppliers
WITH order_analytics AS (
    SELECT
        c.cust_id,
        c.country,
        p.category,
        s.country AS supplier_country,
        DATE_TRUNC('month', o.order_date) AS month,
        COUNT(*) AS order_count,
        SUM(o.amount) AS total_amount,
        AVG(o.amount) AS avg_amount,
        LAG(SUM(o.amount)) OVER (PARTITION BY c.cust_id ORDER BY month) AS prev_month_amount,
        RANK() OVER (PARTITION BY c.country ORDER BY SUM(o.amount) DESC) AS country_rank
    FROM customers c
    JOIN orders o ON c.cust_id = o.cust_id
    JOIN products p ON o.prod_id = p.prod_id
    JOIN suppliers s ON p.prod_id = s.prod_id
    WHERE o.order_date >= '2024-01-01'
    GROUP BY CUBE(c.cust_id, c.country, p.category, supplier_country, month)
    HAVING COUNT(*) > 100
)
SELECT
    country,
    category,
    supplier_country,
    month,
    AVG(total_amount) AS avg_total,
    AVG(avg_amount) AS avg_avg,
    SUM(order_count) AS total_orders,
    AVG(country_rank) AS avg_rank
FROM order_analytics
WHERE month IS NOT NULL
GROUP BY country, category, supplier_country, month
ORDER BY country, month, avg_total DESC
LIMIT 1000;

