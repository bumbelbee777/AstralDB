CREATE TABLE hello (phrase TEXT);
INSERT INTO hello VALUES ('Hello, AstralDB!');
UPDATE hello SET phrase = 'Hello, World!' WHERE phrase = 'Hello, AstralDB!';
DELETE FROM hello WHERE phrase = 'Hello, World!';