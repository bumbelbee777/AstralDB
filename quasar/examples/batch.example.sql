-- Quasar batch: directives apply to following statements until GO or next directive

-- @broadcast
CREATE TABLE IF NOT EXISTS cluster_meta (k TEXT, v TEXT);

GO

-- @shard tenant:acme
INSERT INTO cluster_meta VALUES ('tenant', 'acme');

GO

-- @shard tenant:globex
INSERT INTO cluster_meta VALUES ('tenant', 'globex');
