WITH agg AS (
    SELECT sa_dim.id, COUNT(sa_fact.id) AS cnt, MAX(sa_fact.a) AS peak
    FROM sa_dim
    INNER JOIN sa_fact ON sa_dim.id = sa_fact.id
    GROUP BY sa_dim.id
    HAVING COUNT(sa_fact.id) > 1
)
SELECT id, cnt, peak
FROM agg
ORDER BY cnt DESC
LIMIT 100;
