-- GROUPING SETS and GROUPING() (OLAP extensions)
CREATE TABLE gs (region TEXT, product TEXT, amt INT);
INSERT INTO gs VALUES ('east', 'a', 10), ('east', 'b', 20), ('west', 'a', 30);

SELECT region, product, cnt, GROUPING(region) AS g_region
FROM gs
GROUP BY region, product
GROUPING SETS ((region, product), (region), ());
