-- Time series and analytics helpers (ISO timestamps, bucketing, columnar GROUP BY).

CREATE TABLE metrics (device TEXT, ts TEXT, value INT);
INSERT INTO metrics VALUES
    ('a', '2024-01-01T10:15:30', 1),
    ('a', '2024-01-01T10:45:00', 2),
    ('a', '2024-01-01T11:20:00', 3),
    ('b', '2024-01-01T10:05:00', 4);

WITH hourly AS (
    SELECT device,
           TIME_BUCKET(ts, 3600) AS hour_bucket,
           value
    FROM metrics
)
SELECT device, hour_bucket, SUM(value) AS total
FROM hourly
GROUP BY device, hour_bucket
ORDER BY device ASC, hour_bucket ASC;

WITH daily AS (
    SELECT DATE_TRUNC('day', ts) AS day_start, value FROM metrics
)
SELECT day_start, AVG(value) AS avg_val FROM daily GROUP BY day_start;

SELECT EXTRACT(HOUR FROM ts) AS hr, TIMESTAMP_DIFF(ts, '2024-01-01T10:00:00') AS sec_since
FROM metrics
WHERE device = 'a'
ORDER BY ts ASC;
