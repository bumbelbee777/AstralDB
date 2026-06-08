-- Query 3: Eight overlapping windows
SELECT
    order_id,
    cust_id,
    order_date,
    amount,
    SUM(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS UNBOUNDED PRECEDING) AS running_total,
    AVG(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS BETWEEN 30 PRECEDING AND CURRENT ROW) AS ma30,
    AVG(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS BETWEEN 7 PRECEDING AND CURRENT ROW) AS ma7,
    LAG(amount) OVER (PARTITION BY cust_id ORDER BY order_date) AS prev_amount,
    RANK() OVER (PARTITION BY order_date ORDER BY amount DESC) AS monthly_rank,
    NTILE(10) OVER (PARTITION BY cust_id ORDER BY amount) AS spending_decile,
    SUM(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS BETWEEN 5 PRECEDING AND CURRENT ROW) AS ma5,
    AVG(amount) OVER (PARTITION BY cust_id ORDER BY order_date ROWS BETWEEN 14 PRECEDING AND CURRENT ROW) AS ma14
FROM orders
WHERE order_date >= '2024-01-01'
ORDER BY cust_id, order_date
LIMIT 1000000;

