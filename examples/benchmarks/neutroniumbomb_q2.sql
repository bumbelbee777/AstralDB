-- Query 2: Recursive org + CUBE
WITH RECURSIVE org_tree AS (
    SELECT
        cust_id,
        1 AS level,
        CAST(cust_id AS TEXT) AS path,
        lifetime_value AS base_value
    FROM customers
    WHERE cust_id % 997 = 0
    UNION ALL
    SELECT
        c.cust_id,
        ot.level + 1,
        ot.path || ' -> ' || CAST(c.cust_id AS TEXT),
        ot.base_value * 0.8
    FROM customers c
    JOIN org_tree ot ON c.cust_id = ot.cust_id + 1
    WHERE ot.level < 10
)
SELECT
    level,
    COUNT(*) AS count,
    AVG(base_value) AS avg_influence,
    path
FROM org_tree
GROUP BY CUBE(level, path)
ORDER BY level, count DESC
LIMIT 1000;

