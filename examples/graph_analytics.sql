-- GQL analytics: variable-length paths, shortest path, PageRank, projection
CREATE TABLE users (id INT, name TEXT);
CREATE TABLE follows (src INT, dst INT, kind TEXT);

INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol'), (4, 'Dave'), (5, 'Eve');
INSERT INTO follows VALUES
  (1, 2, 'follows'), (1, 3, 'follows'), (2, 4, 'follows'), (3, 4, 'follows'),
  (4, 5, 'follows'), (5, 1, 'follows');

CREATE GRAPH social
  VERTEX TABLE users (id)
  EDGE TABLE follows (src, dst, kind);

CREATE GRAPH PROJECTION social_follows FROM social EDGE WHERE e.kind = 'follows';

GRAPH MATCH (a)-[e*1 .. 3]->(b) FROM 1 IN social_follows INTO paths_1_3;
GRAPH SHORTEST PATH FROM 1 TO 5 IN social INTO route;
GRAPH PAGERANK IN social DAMPING 0.85 ITERATIONS 25 INTO ranks;

SELECT start_id, end_id, path_length, path FROM paths_1_3 ORDER BY path_length, end_id;
SELECT found, path_length, path, position, vertex_id FROM route ORDER BY position;
SELECT vertex_id, rank FROM ranks ORDER BY rank DESC;
