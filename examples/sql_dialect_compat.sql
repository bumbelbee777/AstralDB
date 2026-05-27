-- Cross-dialect sugar: SQLite, PostgreSQL, and Oracle compatibility (see docs/Overview.md).

CREATE TABLE users (
	id SERIAL PRIMARY KEY,
	name TEXT,
	nick TEXT
);

INSERT INTO users (name, nick) VALUES ('Ada', 'ada');
INSERT INTO users (name, nick) VALUES ('Grace', 'grace');

-- Oracle DUAL + scalar expressions
SELECT 'hello' AS greeting FROM DUAL;

-- SQLite / Oracle null handling
SELECT IFNULL(nick, name) AS display FROM users;
SELECT NVL(nick, name) AS display2 FROM users;
SELECT NVL2(nick, nick, name) AS display3 FROM users;

-- Oracle DECODE
SELECT DECODE(name, 'Ada', 'found', 'other') AS bucket FROM users ORDER BY name;

-- PostgreSQL ILIKE and SQLite GLOB
SELECT name FROM users WHERE name ILIKE 'ad%';
SELECT name FROM users WHERE name GLOB 'G*';

-- MySQL REGEXP and PostgreSQL ~ / REGEXP_MATCH()
SELECT name FROM users WHERE name REGEXP '^A';
SELECT name FROM users WHERE name ~ 'race';
SELECT name FROM users WHERE REGEXP_MATCH(name, '^G');

-- String concat (||)
SELECT name || ' (' || nick || ')' AS label FROM users ORDER BY name;

-- SQLite REPLACE INTO (requires explicit column list when refreshing on PK conflict)
REPLACE INTO users (id, name, nick) VALUES (1, 'Ada Lovelace', 'ada');

-- PostgreSQL :: cast and nested NVL2
SELECT name::TEXT AS name_text, NVL2(nick, UPPER(nick), 'none') AS nick_up FROM users ORDER BY id;

-- Oracle ROWNUM cap (applied as row limit before final ORDER BY)
SELECT name FROM users WHERE ROWNUM <= 1;

-- Oracle CONNECT BY (START WITH + PRIOR parent = child)
CREATE TABLE org (id INT, name TEXT, mgr INT);
INSERT INTO org VALUES (1, 'ceo', NULL), (2, 'eng', 1), (3, 'sales', 1);
SELECT name, mgr FROM org
START WITH mgr IS NULL
CONNECT BY PRIOR id = mgr
ORDER BY name;

-- DuckDB-style integer division, lambdas, and COLUMNS()
CREATE TABLE nums (id INT, a INT, b INT, tags TEXT);
INSERT INTO nums VALUES (1, 10, 3, '[1,2,3]'), (2, 7, 2, '[4,5]');
SELECT id, a // b AS q FROM nums ORDER BY id;
SELECT COLUMNS(*) FROM nums;
SELECT LIST_TRANSFORM('[1,2,3]', x -> x + 1) AS bumped FROM DUAL;

-- SQLite REPLACE INTO without column list (implicit schema columns)
REPLACE INTO users VALUES (1, 'Ada L', 'ada');

-- PostgreSQL RETURNING (materializes into __astral_returning)
INSERT INTO users (name, nick) VALUES ('Turing', 't') RETURNING id, name;
SELECT id, name FROM __astral_returning ORDER BY id;

-- SQLite INTEGER PRIMARY KEY alias to SERIAL/identity
CREATE TABLE pkalias (id INTEGER PRIMARY KEY, v INT);
INSERT INTO pkalias (v) VALUES (42) RETURNING id;
SELECT id FROM __astral_returning;
