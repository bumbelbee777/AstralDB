-- PL/pgSQL-style procedure (lowered to AstralDB SQL, cached as .abc)
CREATE OR REPLACE PROCEDURE seed_pgsql()
LANGUAGE plpgsql
AS $$
BEGIN
  CREATE TABLE proc_pgsql (id INTEGER, note TEXT);
  INSERT INTO proc_pgsql VALUES (1, 'from plpgsql');
END;
$$;

CALL seed_pgsql;
SELECT note FROM proc_pgsql;
