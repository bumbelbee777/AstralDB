-- Query 6: Geo + TS + vector + FTS
SELECT
    o.order_id,
    ST_DISTANCE_SPHERICAL(o.shipping_address, 'POINT(0 0)') AS distance_from_origin,
    TS_DECOMPRESS(i.price_history) AS price_history,
    VECTOR_TOPK('product_embeddings', p.embedding, 5) AS similar_products,
    o.amount,
    JSON_EXTRACT(o.metadata, '$.payment_method') AS payment_method,
    o.review_text MATCH 'good|excellent|great' AS positive_review
FROM orders o
JOIN products p ON o.prod_id = p.prod_id
JOIN inventory i ON p.prod_id = i.prod_id
WHERE ST_WITHIN_BBOX(o.shipping_address, -180, -90, 180, 90)
  AND o.order_date >= '2024-01-01'
  AND JSON_EXTRACT(o.metadata, '$.payment_method') IN ('credit_card', 'paypal')
ORDER BY distance_from_origin, positive_review DESC
LIMIT 100000;

