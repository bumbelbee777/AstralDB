-- Minimal HTAP setup for ACID interrupt/resume demo (scale BULK via harness).
DROP TABLE IF EXISTS web_sessions;
DROP TABLE IF EXISTS payments;
DROP TABLE IF EXISTS inventory;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS customers;

CREATE TABLE customers (
    cust_id INT PRIMARY KEY,
    lifetime_value DECIMAL,
    shipping_address POINT
);

CREATE TABLE products (prod_id INT PRIMARY KEY);

CREATE TABLE orders (
    order_id INT PRIMARY KEY,
    cust_id INT,
    prod_id INT,
    order_date TIMESTAMP,
    quantity INT,
    amount DECIMAL,
    shipping_address POINT
);

CREATE TABLE inventory (prod_id INT PRIMARY KEY, price_history LIST(DOUBLE));

CREATE TABLE payments (
    pay_id INT PRIMARY KEY,
    order_id INT,
    method TEXT,
    amount DECIMAL,
    paid_at TIMESTAMP
);

CREATE TABLE web_sessions (
    sess_id INT PRIMARY KEY,
    cust_id INT,
    page_url TEXT,
    ts TIMESTAMP,
    cart JSON
);

INSERT INTO customers BULK 10000 START 1 STEP 1;
INSERT INTO products BULK 10000 START 1 STEP 1;
INSERT INTO orders BULK 10000 START 1 STEP 1;
INSERT INTO inventory BULK 10000 START 1 STEP 1;
INSERT INTO payments BULK 10000 START 1 STEP 1;
INSERT INTO web_sessions BULK 10000 START 1 STEP 1;
