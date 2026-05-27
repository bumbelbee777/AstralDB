-- Oracle PL/SQL-style procedure (lowered to AstralDB SQL, cached as .abc)
CREATE OR REPLACE PROCEDURE seed_plsql
IS
BEGIN
  CREATE TABLE proc_plsql (id INTEGER, tag TEXT);
  INSERT INTO proc_plsql VALUES (42, 'from plsql');
END seed_plsql;

CALL seed_plsql;
SELECT tag FROM proc_plsql;
