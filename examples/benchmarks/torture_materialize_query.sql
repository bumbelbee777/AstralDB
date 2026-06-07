-- Shape-equivalent to benchmark_torture_unified.sql on lazy BULK id-aligned tables.
-- CTE wrapper required for GROUP BY under --time-sql query-only timing.

WITH agg AS (
    SELECT
        c.id AS id,
        c.b AS name,
        COUNT(*) AS cnt,
        SUM(CAST(li.a AS DOUBLE)) AS total_spent,
        AVG(CAST(li.a AS DOUBLE)) AS avg_order,
        MIN(CAST(li.a AS DOUBLE)) AS min_order,
        MAX(CAST(li.a AS DOUBLE)) AS max_order
    FROM bt_cust c
    INNER JOIN bt_ord o ON c.id = o.id
    INNER JOIN bt_line li ON li.id = o.id
    WHERE c.a > 0
    GROUP BY c.id, c.b
    HAVING COUNT(*) > 0
)
SELECT
    id,
    cnt AS order_count,
    total_spent,
    avg_order,
    min_order,
    max_order,
    CAST(min_order AS DOUBLE) AS min_order_cast,
    COALESCE(name, '') AS display_name,
    CASE WHEN cnt > 5 THEN 1 ELSE 0 END AS heavy_buyer,
    RANK() OVER (ORDER BY total_spent DESC) AS spend_rank
FROM agg
ORDER BY total_spent DESC
LIMIT 100;
