SELECT COUNT(*) AS n
FROM customers c
JOIN orders o ON c.cust_id = o.cust_id;
