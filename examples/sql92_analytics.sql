-- SQL-92-oriented analytics snippets (see docs/Overview.md for semantics and limits).

CREATE TABLE events (user_id INT, day_id INT);
INSERT INTO events VALUES (1, 1), (1, 2), (1, 2), (2, 1);
SELECT user_id, COUNT(DISTINCT day_id) FROM events GROUP BY user_id ORDER BY user_id ASC;

CREATE TABLE dims (region TEXT, metric INT);
INSERT INTO dims VALUES ('east', 10), ('west', 5);
SELECT region, COALESCE(NULL, metric, 0) AS metric_filled FROM dims ORDER BY region ASC, metric_filled ASC;

CREATE TABLE scores (player TEXT, pts INT);
INSERT INTO scores VALUES ('a', 10), ('b', 10), ('c', 20), ('d', 30);
SELECT player, pts, RANK() OVER (ORDER BY pts DESC) AS rk FROM scores ORDER BY pts DESC, player ASC;

CREATE TABLE sub_demo (s TEXT);
INSERT INTO sub_demo VALUES ('abcdef');
SELECT SUBSTRING(s FROM 3) AS tail, SUBSTRING(s FROM 2 FOR 3) AS mid FROM sub_demo;
