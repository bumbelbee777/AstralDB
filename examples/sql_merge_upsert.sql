-- MERGE and INSERT ... ON CONFLICT (UPSERT) examples for AstralDB.
-- MERGE requires: WHEN MATCHED / WHEN NOT MATCHED branches, join ON target_alias.key = source_alias.key.

CREATE TABLE dim (id INT PRIMARY KEY, label TEXT);
CREATE TABLE staging (sk INT, label TEXT);

INSERT INTO dim VALUES (1, 'old');
INSERT INTO staging VALUES (1, 'from_staging');
INSERT INTO staging VALUES (2, 'new_row');

MERGE INTO dim AS d
USING staging AS s
ON d.id = s.sk
WHEN MATCHED THEN
  UPDATE SET label = s.label
WHEN NOT MATCHED THEN
  INSERT (id, label) VALUES (s.sk, s.label);

INSERT INTO dim (id, label) VALUES (3, 'x')
ON CONFLICT (id) DO UPDATE SET label = EXCLUDED.label;

INSERT INTO dim (id, label) VALUES (3, 'ignored')
ON CONFLICT DO NOTHING;

-- MERGE with only WHEN NOT MATCHED (insert new keys from staging).
MERGE INTO dim AS d
USING staging AS s
ON d.id = s.sk
WHEN NOT MATCHED THEN
  INSERT (id, label) VALUES (s.sk, s.label);
