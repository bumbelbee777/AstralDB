-- Finance graph: weighted shortest path + payment projection + counterparty reach
CREATE TABLE accounts (id INT, name TEXT);
CREATE TABLE transfers (src INT, dst INT, kind TEXT, amount INT);

INSERT INTO accounts VALUES (100, 'Treasury'), (101, 'Alice'), (102, 'Bob'), (103, 'Carol');
INSERT INTO transfers VALUES
  (100, 101, 'fund', 5000),
  (101, 102, 'payment', 120),
  (102, 103, 'payment', 80),
  (103, 101, 'payment', 40);

CREATE GRAPH ledger
  VERTEX TABLE accounts (id)
  EDGE TABLE transfers (src, dst, kind)
  WEIGHT (amount);

CREATE GRAPH PROJECTION payments FROM ledger EDGE WHERE e.kind = 'payment';

GRAPH MATCH (a)-[e]->(b) IN payments INTO payment_edges;
GRAPH MATCH (a)-[e*1 .. 4]->(b) FROM 101 IN payments INTO reach;
GRAPH SHORTEST PATH FROM 101 TO 103 WEIGHTED IN ledger INTO cheapest;
GRAPH PAGERANK IN payments ITERATIONS 30 INTO influence;

SELECT src_id, dst_id FROM payment_edges ORDER BY src_id, dst_id;
SELECT start_id, end_id, path_length FROM reach ORDER BY path_length, end_id;
SELECT found, path_length, path FROM cheapest;
SELECT vertex_id, rank FROM influence ORDER BY rank DESC;
