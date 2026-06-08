-- Query 7: Recursive + windows + FTS
WITH RECURSIVE related_products AS (
    SELECT
        prod_id,
        name,
        category,
        1 AS depth,
        CAST(prod_id AS TEXT) AS path
    FROM products
    WHERE category = 'electronics'
    UNION ALL
    SELECT
        p.prod_id,
        p.name,
        p.category,
        rp.depth + 1,
        rp.path || CAST(p.prod_id AS TEXT)
    FROM products p
    JOIN related_products rp ON p.category = rp.category AND p.prod_id != rp.prod_id
    WHERE rp.depth < 5
),
scored_products AS (
    SELECT
        rp.prod_id,
        rp.name,
        rp.category,
        rp.depth,
        AVG(o.amount) OVER (PARTITION BY rp.prod_id) AS avg_order_value,
        SUM(o.quantity) OVER (PARTITION BY rp.prod_id) AS total_quantity,
        RANK() OVER (ORDER BY AVG(o.amount) DESC) AS score_rank,
        CASE
            WHEN rp.name MATCH 'premium|deluxe|pro' THEN 1.5
            WHEN rp.name MATCH 'budget|economy|basic' THEN 0.5
            ELSE 1.0
        END AS name_score
    FROM related_products rp
    JOIN orders o ON rp.prod_id = o.prod_id
    WHERE o.order_date >= '2024-01-01'
    GROUP BY rp.prod_id, rp.name, rp.category, rp.depth
)
SELECT
    prod_id,
    name,
    category,
    depth,
    avg_order_value,
    total_quantity,
    score_rank,
    name_score,
    avg_order_value * name_score AS final_score
FROM scored_products
WHERE final_score > 1000
ORDER BY final_score DESC, depth
LIMIT 10000;

