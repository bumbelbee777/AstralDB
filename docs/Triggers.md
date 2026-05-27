# Triggers

Triggers are **storage-backed**: registry JSON (**`astraldb_triggers.json`**), compiled bodies under **`astraldb_triggers_cache/<name>.abc`**, WAL lines **`TR|` / `TD|` / `TE|` / `TX|`**, and a **`<<<ASTRAL_DB_TRIGGERS>>>`** snapshot trailer on checkpoint. Recent fires are kept in **`recent_fires`** (ring buffer, last 128 events).

## SQL surface

```sql
CREATE TRIGGER bump
  AFTER INSERT ON orders
  FOR EACH ROW
  AS (UPDATE audit SET n = n + 1;);

ALTER TRIGGER bump DISABLE;
ALTER TRIGGER bump ENABLE;
DROP TRIGGER bump;
```

Row context for trigger bodies is exposed in scratch tables **`__astral_trig_new`** and **`__astral_trig_old`** (one row each when applicable). **`EXECUTE PROCEDURE p`** / **`CALL p`** delegate to a registered procedure.

## CLI introspection

```text
astraldb --trig-list -m
astraldb --trig-info orders_ai -m
astraldb --trig-relations -m
astraldb --trig-fires -m
```

Flag reference: [`Usage.md`](Usage.md).

## Example

[`examples/sql_trigger.sql`](../examples/sql_trigger.sql)
