-- SQL user catalog management (CREATE / ALTER / DROP USER; see docs/Overview.md).

CREATE USER IF NOT EXISTS app_reader IDENTIFIED BY 'read_secret';
CREATE USER app_writer IDENTIFIED BY 'write_secret';

CREATE TABLE secrets (id INT, label TEXT);
INSERT INTO secrets VALUES (1, 'alpha');

GRANT SELECT ON secrets TO app_reader;
GRANT SELECT, INSERT, UPDATE ON secrets TO app_writer;

ALTER USER app_reader IDENTIFIED BY 'read_secret_v2';

DROP USER IF EXISTS app_writer;
