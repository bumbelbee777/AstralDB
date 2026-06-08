-- Query 4: Graph match
CREATE GRAPH social_graph
    VERTEX TABLE customers (cust_id)
    EDGE TABLE orders (cust_id, prod_id);

GRAPH MATCH (c1)-[:ordered*1..5]->(p)-[:ordered*1..5]->(c2)
FROM 1 IN social_graph
WHERE c1.cust_id = 1
INTO connected_customers;

SELECT
    end_id,
    path_length AS distance,
    COUNT(*) AS connection_strength
FROM connected_customers
GROUP BY end_id, path_length
ORDER BY distance, connection_strength DESC
LIMIT 1000;

