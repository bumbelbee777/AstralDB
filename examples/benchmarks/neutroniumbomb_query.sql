-- Query 10: HTAP warehouse megafusion (join + geo + MathSci tail)
WITH all_features AS (
    SELECT
        c.cust_id,
        c.lifetime_value,
        o.amount,
        o.quantity,
        o.order_date,
        ST_DISTANCE(o.shipping_address, 'POINT(0 0)') AS distance,
        TS_COMPRESS(i.price_history) AS compressed_prices,
        MCTS_SEARCH('L[4]:0.25,0.25,0.25,0.25', 'L[4]:1,1,1,1', 100, 1.414) AS action
    FROM customers c
    JOIN orders o ON c.cust_id = o.cust_id
    JOIN products p ON o.prod_id = p.prod_id
    JOIN inventory i ON p.prod_id = i.prod_id
    WHERE o.order_date >= '2024-01-01'
      AND ST_WITHIN_BBOX(o.shipping_address, -180, -90, 180, 90)
)
SELECT
    order_date AS month,
    AVG(lifetime_value) AS avg_lifetime,
    SUM(amount) AS total_revenue,
    AVG(distance) AS avg_distance,
    COUNT(*) AS row_count,
    MAX(action) AS sample_action
FROM all_features
GROUP BY order_date
ORDER BY order_date DESC
LIMIT 1000;
