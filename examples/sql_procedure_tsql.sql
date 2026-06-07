-- T-SQL-style procedure (lowered to AstralDB SQL, cached as .abc)
CREATE OR ALTER PROCEDURE seed_tsql
AS
BEGIN
  SET NOCOUNT ON;
  CREATE TABLE proc_tsql (id INTEGER, tag TEXT);
  INSERT INTO proc_tsql VALUES (7, 'from tsql');
END;

CALL seed_tsql;
SELECT tag FROM proc_tsql;
