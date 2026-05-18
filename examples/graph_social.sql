-- Minimal GQL: social follow graph (1-hop match + multi-hop BFS traverse)
CREATE TABLE users (id INT, name TEXT);
CREATE TABLE follows (src INT, dst INT);

INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol'), (4, 'Dave');
INSERT INTO follows VALUES (1, 2), (1, 3), (2, 4), (3, 4);

CREATE GRAPH social
  VERTEX TABLE users (id)
  EDGE TABLE follows (src, dst);

GRAPH MATCH (a)-[e]->(b) IN social INTO all_edges;
GRAPH TRAVERSE FROM 1 IN social DEPTH 3 BFS INTO reach_alice;

SELECT src_id, dst_id FROM all_edges ORDER BY src_id, dst_id;
SELECT vertex_id, depth FROM reach_alice ORDER BY depth, vertex_id;
