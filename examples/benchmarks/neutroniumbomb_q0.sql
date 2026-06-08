-- Query 0: OLTP churn
BEGIN;
INSERT INTO payments VALUES (9000000001, 1, 'credit_card', 99.99, '2024-06-01');
INSERT INTO web_sessions VALUES (9000000001, 1, '/checkout', '2024-06-01', '{"items":1}');
UPDATE payments SET amount = 100.00 WHERE pay_id = 9000000001;
COMMIT;

