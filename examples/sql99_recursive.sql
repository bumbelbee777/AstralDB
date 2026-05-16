-- A real recursive CTE (tree traversal)
CREATE TABLE employees (
    id INT,
    name TEXT,
    manager_id INT
);

INSERT INTO employees VALUES 
    (1, 'Alice', NULL),
    (2, 'Bob', 1),
    (3, 'Carol', 1),
    (4, 'David', 2);

WITH RECURSIVE org_tree AS (
    SELECT id, name, manager_id, 0 AS level
    FROM employees WHERE manager_id IS NULL
    UNION ALL
    SELECT e.id, e.name, e.manager_id, ot.level + 1
    FROM employees e
    JOIN org_tree ot ON e.manager_id = ot.id
)
SELECT * FROM org_tree ORDER BY level, id;