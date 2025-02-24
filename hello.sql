CREATE TABLE hello (phrase TEXT)
INSERT INTO hello VALUES ('Hello, AstralDB!')
FROM hello WHERE phrase = 'Hello, AstralDB!'
UPDATE hello SET phrase = 'Hello, World!'
DELETE hello