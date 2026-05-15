-- RBAC surface (runs without session auth; user/role membership grants need catalog users via API)
CREATE TABLE emp ( id INT PRIMARY KEY, name TEXT, salary TEXT );
INSERT INTO emp VALUES (1, 'alice', '100');
INSERT INTO emp VALUES (2, 'bob', '200');

CREATE ROLE analyst;
GRANT SELECT ON emp TO ROLE analyst;
