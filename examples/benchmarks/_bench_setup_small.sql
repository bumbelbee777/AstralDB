-- Small warehouse for local _bench_* timing (100k rows/table).

DROP TABLE IF EXISTS inventory;

DROP TABLE IF EXISTS events;

DROP TABLE IF EXISTS orders;

DROP TABLE IF EXISTS products;

DROP TABLE IF EXISTS customers;



CREATE TABLE customers (

    cust_id INT PRIMARY KEY,

    name TEXT,

    country TEXT,

    signup_date TIMESTAMP,

    lifetime_value DECIMAL,

    profile JSON,

    xml_data TEXT,

    bio TEXT

);



CREATE TABLE products (

    prod_id INT PRIMARY KEY,

    name TEXT,

    category TEXT,

    price DECIMAL

);



CREATE TABLE orders (

    order_id INT PRIMARY KEY,

    cust_id INT,

    prod_id INT,

    order_date TIMESTAMP,

    quantity INT,

    amount DECIMAL,

    status TEXT,

    review_text TEXT,

    metadata JSON

);



CREATE TABLE events (

    event_id INT PRIMARY KEY,

    cust_id INT,

    event_type TEXT,

    event_time TIMESTAMP

);



CREATE TABLE inventory (

    prod_id INT PRIMARY KEY,

    stock INT,

    last_restock TIMESTAMP

);



INSERT INTO customers BULK 100000 START 1 STEP 1;

INSERT INTO products BULK 100000 START 1 STEP 1;

INSERT INTO orders BULK 100000 START 1 STEP 1;

INSERT INTO events BULK 100000 START 1 STEP 1;

INSERT INTO inventory BULK 100000 START 1 STEP 1;

