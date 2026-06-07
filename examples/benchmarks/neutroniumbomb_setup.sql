-- neutroniumbomb bench — phase 1: ten-table synthetic HTAP warehouse (scaled via harness BULK rewrite).
-- Run setup only: astraldb -m -O4 -f examples/benchmarks/neutroniumbomb_setup.sql

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
