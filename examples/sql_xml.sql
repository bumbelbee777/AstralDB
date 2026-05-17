-- SQL/XML scalars: parse, extract paths, canonical serialize.

CREATE TABLE docs (id INT, body TEXT);

INSERT INTO docs VALUES
	(1, '<book id="42"><title>Astral</title><author>DB</author></book>'),
	(2, '<note>plain</note>');

SELECT
	XML_VALID(body) AS ok,
	XML_EXTRACT(body, 'book.title') AS title,
	XML_EXTRACT(body, 'book/@id') AS book_id,
	XML_SERIALIZE(body) AS canon
FROM docs
WHERE id = 1;

SELECT XML_EXTRACT(body, 'note') AS txt FROM docs WHERE id = 2;
