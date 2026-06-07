-- HTAP warehouse megasuite — 10 tables × 1B rows (scale via harness BULK rewrite).
-- Smoke: 100M/table. Full: 1B/table. Run: astraldb -m -O4 --time-sql-setup examples/benchmarks/neutroniumbomb_setup.sql --time-sql-suite examples/benchmarks/neutroniumbomb_suite.txt

-- Phase 1: DDL + synthetic bulk (see examples/benchmarks/neutroniumbomb_setup.sql for split harness)

DROP TABLE IF EXISTS web_sessions;
DROP TABLE IF EXISTS suppliers;
DROP TABLE IF EXISTS returns;
DROP TABLE IF EXISTS shipments;
DROP TABLE IF EXISTS payments;
DROP TABLE IF EXISTS inventory;
DROP TABLE IF EXISTS events;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS customers;
DROP TABLE IF EXISTS initial_weights;
DROP TABLE IF EXISTS training_data;
DROP TABLE IF EXISTS nfp_params;

CREATE TABLE customers (
    cust_id INT PRIMARY KEY,
    name TEXT,
    country TEXT,
    signup_date TIMESTAMP,
    lifetime_value DECIMAL,
    embedding VECTOR(128),
    profile JSON,
    bio TEXT,
    xml_data XML
);

CREATE TABLE products (
    prod_id INT PRIMARY KEY,
    name TEXT,
    category TEXT,
    price DECIMAL,
    tags LIST(TEXT),
    embedding VECTOR(128)
);

CREATE TABLE orders (
    order_id INT PRIMARY KEY,
    cust_id INT,
    prod_id INT,
    order_date TIMESTAMP,
    quantity INT,
    amount DECIMAL,
    status TEXT,
    shipping_address POINT,
    review_text TEXT,
    metadata JSON
);

CREATE TABLE events (
    event_id INT PRIMARY KEY,
    cust_id INT,
    event_type TEXT,
    event_time TIMESTAMP,
    metadata JSON,
    page_url TEXT
);

CREATE TABLE inventory (
    prod_id INT PRIMARY KEY,
    stock INT,
    warehouse POINT,
    last_restock TIMESTAMP,
    price_history LIST(DOUBLE)
);

CREATE TABLE payments (
    pay_id INT PRIMARY KEY,
    order_id INT,
    method TEXT,
    amount DECIMAL,
    paid_at TIMESTAMP
);

CREATE TABLE shipments (
    ship_id INT PRIMARY KEY,
    order_id INT,
    origin POINT,
    dest POINT,
    eta TIMESTAMP,
    route LIST(DOUBLE)
);

CREATE TABLE returns (
    ret_id INT PRIMARY KEY,
    order_id INT,
    reason JSON,
    notes TEXT
);

CREATE TABLE suppliers (
    sup_id INT PRIMARY KEY,
    prod_id INT,
    country TEXT,
    lead_days INT
);

CREATE TABLE web_sessions (
    sess_id INT PRIMARY KEY,
    cust_id INT,
    page_url TEXT,
    ts TIMESTAMP,
    cart JSON
);

INSERT INTO customers BULK 1000000000 START 1 STEP 1;
INSERT INTO products BULK 1000000000 START 1 STEP 1;
INSERT INTO orders BULK 1000000000 START 1 STEP 1;
INSERT INTO events BULK 1000000000 START 1 STEP 1;
INSERT INTO inventory BULK 1000000000 START 1 STEP 1;
INSERT INTO payments BULK 1000000000 START 1 STEP 1;
INSERT INTO shipments BULK 1000000000 START 1 STEP 1;
INSERT INTO returns BULK 1000000000 START 1 STEP 1;
INSERT INTO suppliers BULK 1000000000 START 1 STEP 1;
INSERT INTO web_sessions BULK 1000000000 START 1 STEP 1;

CREATE TABLE initial_weights (weights TEXT);
INSERT INTO initial_weights VALUES ('LW[2]:T[2,2]:0.1,0.2,0.3,0.4;T[1,2]:0.5,0.6');
CREATE TABLE training_data (x TEXT, target TEXT, gradient TEXT);
INSERT INTO training_data VALUES ('1,2', '0.5', '0.01,0.02');
CREATE TABLE nfp_params (
    g0 TEXT, grid TEXT, drift TEXT, diffusion TEXT,
    neural_corr TEXT, aggK TEXT, kStar TEXT, barrierA TEXT, dt TEXT, steps TEXT
);
INSERT INTO nfp_params VALUES (
    'L[8]:1,1,1,1,1,1,1,1',
    'L[32]:0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1,1.1,1.2,1.3,1.4,1.5,1.6,1.7,1.8,1.9,2,2.1,2.2,2.3,2.4,2.5,2.6,2.7,2.8,2.9,3',
    'L[32]:0.01', 'L[32]:0.02', 'L[32]:0', '0.5', '1', '0.1', '0.01', '100'
);

-- ============================================================================
-- Phase 2: HTAP terror queries
-- ============================================================================

-- Query 0: OLTP churn
BEGIN;
INSERT INTO payments VALUES (9000000001, 1, 'credit_card', 99.99, '2024-06-01');
INSERT INTO web_sessions VALUES (9000000001, 1, '/checkout', '2024-06-01', '{"items":1}');
UPDATE payments SET amount = 100.00 WHERE pay_id = 9000000001;
COMMIT;

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

-- Query 5: PINN training
SELECT
    epoch,
    loss,
    PREDICT(model, x) AS prediction
FROM (
    WITH RECURSIVE pinn_train(epoch, model, loss) AS (
        SELECT
            0,
            MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', weights),
            1.0
        FROM initial_weights
        UNION ALL
        SELECT
            epoch + 1,
            OPTIMIZER_STEP(model, gradient),
            MSE_LOSS(PREDICT(model, x), target)
        FROM pinn_train, training_data
        WHERE epoch < 100
    )
    SELECT * FROM pinn_train
) WHERE epoch % 10 = 0
ORDER BY epoch;

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

-- Query 8: MCTS + NFP
WITH customer_features AS (
    SELECT
        o.cust_id,
        c.lifetime_value,
        COUNT(*) AS order_count,
        AVG(o.amount) AS avg_amount,
        STDDEV_POP(o.amount) AS amount_volatility
    FROM orders o
    JOIN customers c ON o.cust_id = c.cust_id
    GROUP BY o.cust_id, c.lifetime_value
),
policy_search AS (
    SELECT
        cust_id,
        MCTS_SEARCH(
            'L[4]:0.25,0.25,0.25,0.25',
            'L[4]:1,1,1,1',
            1000,
            1.414
        ) AS optimal_action
    FROM customer_features
    WHERE lifetime_value > 1000
),
wealth_distribution AS (
    SELECT
        NFP_MACRO_MARCH(
            g0, grid, drift, diffusion,
            neural_corr, aggK, kStar, barrierA, dt, steps
        ) AS g_final,
        NFP_MACRO_MOMENTS(g_final, grid) AS moments
    FROM policy_search, nfp_params
)
SELECT
    moments.mass,
    moments.mean,
    moments.variance,
    moments.gini,
    optimal_action
FROM wealth_distribution
ORDER BY moments.gini DESC
LIMIT 1000;

-- Query 9: Semistructured
SELECT
    cust_id,
    JSON_EXTRACT(profile, '$.preferences.theme') AS theme,
    JSON_EXTRACT(profile, '$.preferences.notifications.email') AS email_notify,
    XML_EXTRACT(xml_data, '/customer/settings/language') AS language,
    REGEXP_EXTRACT(bio, '([A-Z][a-z]+ [A-Z][a-z]+)') AS proper_name,
    LENGTH(bio) AS bio_length,
    TEXT_RANK(bio, 'important keywords') AS relevance
FROM customers
WHERE JSON_EXTRACT(profile, '$.active') = true
  AND XML_VALID(xml_data) = 1
  AND bio MATCH 'SELECT|INSERT|UPDATE|DELETE'
ORDER BY relevance DESC
LIMIT 100000;

-- Query 10: HTAP warehouse megafusion
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

-- Query 11: HTAP finale (ACID interrupt/resume demo target)
BEGIN;
INSERT INTO payments VALUES (9000000002, 2, 'paypal', 49.99, '2024-06-02');
INSERT INTO web_sessions VALUES (9000000002, 2, '/cart', '2024-06-02', '{"items":2}');
UPDATE payments SET amount = 50.00 WHERE pay_id = 9000000002;
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
COMMIT;
