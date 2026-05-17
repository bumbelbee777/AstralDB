-- SQL-92/99/03 polish: NULLIF, GREATEST, JSON, AS OF, MATCH_RECOGNIZE.

CREATE TABLE events (
	id INT,
	valid_from TEXT,
	valid_to TEXT,
	kind TEXT,
	amt DOUBLE
);

INSERT INTO events VALUES
	(1, '1704067200', '1706745600', 'A', 10),
	(2, '1704153600', '', 'B', 20),
	(3, '1704240000', '', 'B', 30);

SELECT id, kind, amt
FROM events FOR SYSTEM_TIME AS OF '1704200000'
ORDER BY id;

SELECT NULLIF(amt, 20) AS nz, GREATEST(amt, 15) AS hi FROM events;

SELECT JSON_EXTRACT('{"x":42}', 'x') AS jx, JSON_KEYS('{"a":1,"b":2}') AS keys;

SELECT *
FROM events
MATCH_RECOGNIZE (
	ORDER BY id
	PATTERN (A B+)
	DEFINE
		A AS kind = 'A',
		B AS kind = 'B'
);
