-- Demonstrates core DML AstralDB supports today (subset of full SQL).

CREATE TABLE users (
    id INTEGER,
    username VARCHAR,
    email VARCHAR,
    age INTEGER,
    active INTEGER
);

CREATE TABLE posts (
    id INTEGER,
    user_id INTEGER,
    title VARCHAR,
    views INTEGER
);

CREATE TABLE comments (
    id INTEGER,
    post_id INTEGER,
    user_id INTEGER,
    content VARCHAR
);

INSERT INTO users (id, username, email, age, active) VALUES
    (1, 'john_doe', 'john@example.com', 25, 1),
    (2, 'jane_smith', 'jane@example.com', 30, 1),
    (3, 'bob_wilson', 'bob@example.com', 35, 0);

INSERT INTO users (username, email, age, active, id) VALUES
    ('sys', 'system@example.com', 99, 0, 99);

INSERT INTO posts VALUES
    (1, 1, 'First Post', 100),
    (2, 1, 'Second Post', 50),
    (3, 2, 'My First Post', 75);

INSERT INTO comments VALUES
    (1, 1, 2, 'Great post!'),
    (2, 1, 3, 'Thanks');

SELECT id, username, email, age, active FROM users;
SELECT id, user_id, title, views FROM posts ORDER BY views DESC;

UPDATE users SET age = 31 WHERE username = 'john_doe';
UPDATE posts SET views = 110 WHERE id = 1;

DELETE FROM comments WHERE id = 2;
DELETE FROM posts WHERE user_id = 3;
DELETE FROM users WHERE active = 0;

CREATE TABLE uniq_rows (slot INTEGER);
INSERT INTO uniq_rows VALUES (1), (1), (2);
SELECT DISTINCT slot FROM uniq_rows ORDER BY slot ASC;

GRANT SELECT ON users TO jane_smith;
GRANT INSERT ON posts TO john_doe;
REVOKE INSERT ON posts FROM john_doe;

DROP TABLE uniq_rows;
DROP TABLE posts;
DROP TABLE users;
DROP TABLE comments;
