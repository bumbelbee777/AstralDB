-- Cypher-style MATCH (same semantics as GRAPH MATCH … INTO)
CREATE TABLE users (id INT, name TEXT);
CREATE TABLE follows (src INT, dst INT);
INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');
INSERT INTO follows VALUES (1, 2), (2, 3);

CREATE GRAPH social VERTEX TABLE users (id) EDGE TABLE follows (src, dst);

MATCH (a)-[e]->(b) IN social INTO cypher_edges;
MATCH (a)-[e]->(b) IN social FROM 1 INTO cypher_from_alice;
