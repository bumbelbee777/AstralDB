-- Storage-backed triggers: catalog (astraldb_triggers.json), bytecode cache, WAL, snapshot trailer.
CREATE TABLE orders (id INTEGER, amount INTEGER);
CREATE TABLE order_audit (id INTEGER, note TEXT);

CREATE TRIGGER orders_ai
  AFTER INSERT ON orders
  FOR EACH ROW
  AS (
    INSERT INTO order_audit VALUES (1, 'inserted');
  );

INSERT INTO orders VALUES (1, 100);

-- Introspection (CLI): astraldb --trig-list --trig-info orders_ai --trig-fires
-- Reversible disable (definition kept):
-- ALTER TRIGGER orders_ai DISABLE;
-- ALTER TRIGGER orders_ai ENABLE;
-- DROP TRIGGER orders_ai;
