-- Unified analytic workload (SQL-92-style surface documented in docs/Overview.md).
-- Used by scripts/benchmark_torture_plot.py together with engine-specific DDL/DML
-- so AstralDB, DuckDB, MySQL, and SQLite run the same logical query on the same row counts.
--
-- This file is not under examples/*.sql (root) so CI doesn't execute it without DDL;
-- the plot script loads it explicitly.

WITH agg AS (
    SELECT
        c.id AS id,
        c.name AS name,
        COUNT(*) AS cnt,
        SUM(li.amount) AS total_spent,
        AVG(li.amount) AS avg_order,
        MIN(li.amount) AS min_order,
        MAX(li.amount) AS max_order
    FROM customers c
    INNER JOIN orders o ON o.customer_id = c.id
    INNER JOIN line_items li ON li.order_id = o.id
    WHERE c.created_at > '2026-01-01'
    GROUP BY c.id, c.name
    HAVING COUNT(*) > 5
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
