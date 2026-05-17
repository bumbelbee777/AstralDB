# Quasar (v1.0)

**Quasar** is AstralDB’s Python orchestration layer: a small toolkit (~2 MB with optional Flask) that wraps the **`astraldb` / `astraldb_cli` executable** so you can run sharded clusters, replicas, backups, and migrations without building a custom server.

AstralDB itself stays a **single static binary** with no network listener. Quasar does not change the engine; it **spawns the CLI** with `--database`, `-q`, `-s`, and bundle export/import flags, then coordinates multiple database files on disk.

## Quick start

```bash
# Build AstralDB first (see README)
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release
cmake --build build-ci

pip install -r quasar/requirements.txt
export QUASAR_ASTRALDB=build-ci/astraldb_cli   # or astraldb_cli.exe on Windows

python -m quasar init ./my-cluster
python -m quasar validate ./my-cluster/cluster.json
python -m quasar health ./my-cluster/cluster.json
```

`init` creates `data/`, `backups/`, and a starter `cluster.json` with three shards.

## Architecture

```text
                    ┌─────────────────┐
  App / cron / k8s  │  Quasar CLI or  │
  HTTP gateway      │  Python API     │
                    └────────┬────────┘
                             │ PooledAstralDBClient (batched -q)
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
        shard0.db      shard1.db      shard2.db
        shard0.db.wal  …              …
```

| Component | Role |
|-----------|------|
| `AstralDBClient` | Runs `-q`, `-s`, `--export-bundle`, `--import-bundle` |
| `ConsistentHashRing` | Routes `shard_key` → shard name |
| `QuasarShard` | One SQL target or fan-out to all shards |
| `QuasarReplica` | Master + file replicas (same SQL on each file) |
| `QuasarBackup` | Versioned copy of `.db`, `.wal`, procedure cache |
| `QuasarCluster` | JSON-driven combo of the above |
| `QuasarInventory` | Per-shard disk usage and optional SHA-256 fingerprints |
| `QuasarDrift` | Detect divergent shards (file hash and/or probe SQL) |
| `QuasarSqlBatch` | Run `.sql` with `-- @shard KEY` / `-- @broadcast` directives |
| `QuasarRolling` | Apply DDL/DML one shard at a time |
| `QuasarWatch` | Interval health (+ optional `backup-all`) |
| `QuasarMultiMaster` | Quorum writes across multiple master DB files per shard |
| `QuasarFailover` | Auto-promote standby when primary is unhealthy |
| `QuasarRebalance` | Plan/apply row moves when the hash ring changes |
| `QuasarCrossShardJoin` | Fan-out bundle export + in-memory hash join |
| `PooledAstralDBClient` | Batched subprocesses + worker pool (default when `pool.enabled`) |
| `CrossShardTransaction` | `BEGIN` / `COMMIT` / `ROLLBACK` across shards |
| `QuasarMultiRegion` | Primary region writes + async geo-replication |
| `WorkloadGuard` | Per-shard circuit breakers under load |
| Gateway (optional) | Flask `/query`, `/health`, `/ready`, `/metrics`, `/shards`, `/ring`, `/batch`, `/cross-join`, `/failover/run` |

## Cluster configuration

Example (`quasar/examples/cluster.example.json`):

```json
{
  "shards": [
    { "name": "shard0", "database": "data/shard0.db" },
    { "name": "shard1", "database": "data/shard1.db" },
    { "name": "shard2", "database": "data/shard2.db" }
  ],
  "virtual_nodes": 128,
  "backup_dir": "backups",
  "backup_retention": { "keep_last": 14, "keep_days": 30 },
  "replicas": {
    "shard0_mirror": {
      "master": "data/shard0.db",
      "slaves": ["data/shard0_replica.db"]
    }
  }
}
```

Paths are resolved **relative to the config file’s directory**. Validate with:

```bash
python -m quasar validate my-cluster/cluster.json
```

Environment:

| Variable | Purpose |
|----------|---------|
| `QUASAR_ASTRALDB` / `ASTRALDB_BIN` | Path to `astraldb` / `astraldb_cli` |
| `ASTRALDB_USER` / `ASTRALDB_PASSWORD` | Passed as `-U` / `-P` (same as CLI) |
| `QUASAR_API_KEY` | Optional gateway auth (`X-Quasar-Key` header) |

## CLI reference

Global flags: `--astraldb PATH`, `-U`, `-P`, `-O2` (optimization level).

| Command | Description |
|---------|-------------|
| `version` | Quasar + AstralDB versions |
| `init [root]` | Scaffold `cluster.json`, `data/`, `backups/` |
| `validate CONFIG` | Check cluster JSON |
| `health CONFIG` | Probe all shards (`exit 1` if unhealthy) |
| `query DB "SQL"` | Single-database query |
| `shard CONFIG "SQL" [--shard-key K]` | Routed or broadcast SQL |
| `script FILE [--database DB \| --config CONFIG] [--shard-key K]` | Run `.sql` file |
| `replicate MASTER [REPLICAS...] "SQL" [--parallel]` | Write to master + replicas |
| `backup DB BACKUP_DIR [--version LABEL] [--incremental]` | Full or WAL-only backup |
| `backup-all CONFIG [--incremental] [--prefix P]` | Backup every shard |
| `backup-list BACKUP_DIR [--json]` | List versions |
| `backup-prune BACKUP_DIR [--keep-last N] [--keep-days D] [--dry-run]` | Retention |
| `restore BACKUP_DIR VERSION TARGET_DB [--overwrite]` | Restore files |
| `migrate SOURCE TARGET [--copy] [--work-dir DIR]` | Bundle or file copy |
| `gateway CONFIG [--host H] [--port P] [--api-key K]` | HTTP front door |
| `status CONFIG [--hash]` | Health + disk inventory JSON |
| `drift CONFIG [--probe-sql SQL] [--no-hash]` | Shard consistency check (`exit 1` if drift) |
| `ring CONFIG KEY [KEY...]` | Show which shard owns each key |
| `rolling CONFIG "SQL" [--delay SEC]` | Sequential shard execution |
| `batch CONFIG SCRIPT.sql` | Annotated multi-shard SQL file |
| `watch CONFIG [--interval SEC] [--backup] [--iterations N]` | Health/backup loop |
| `repair-replica CONFIG REPLICA_SET` | Recopy master → slaves from config |
| `compile SQL.sql [-o out.abc] [--pool]` | Wrapper for `astraldb -cc` |
| `multi-master CONFIG "SQL" --shard-key K [--read]` | Quorum write or read from master group |
| `failover CONFIG {status\|run\|reset}` | Failover state and promotion |
| `rebalance CONFIG SHARD... [--apply] [--plan-file F]` | Plan/apply automatic rebalancing |
| `cross-join CONFIG spec.json [-o out.json]` | Cross-shard JOIN |
| `pool-stats CONFIG` | Batching / pool counters |
| `workload-stats CONFIG` | Circuit breaker state |
| `xtxn CONFIG --spec FILE` | Cross-shard transaction |
| `region CONFIG "SQL" --shard-key K [--read] [--region NAME]` | Multi-region I/O |

Examples:

```bash
# Sharded insert (application supplies shard key)
python -m quasar shard my-cluster/cluster.json \
  "CREATE TABLE users (id INT, name TEXT);" --shard-key "user:42"

# Broadcast schema to every shard
python -m quasar shard my-cluster/cluster.json \
  "CREATE TABLE IF NOT EXISTS meta (k TEXT, v TEXT);"

# Nightly backups + retention from config
python -m quasar backup-all my-cluster/cluster.json

# Replication
python -m quasar replicate data/primary.db data/replica.db \
  "INSERT INTO events VALUES (1, 'ping');"

# HTTP gateway (dev / sidecar)
python -m quasar gateway my-cluster/cluster.json --port 8080
curl -s http://127.0.0.1:8080/ready
curl -s -X POST http://127.0.0.1:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT 1;","shard_key":"user:1"}'
```

Prometheus text format: `GET /metrics?format=prometheus`.

Additional gateway routes:

| Route | Description |
|-------|-------------|
| `GET /shards` | Shard names and database paths |
| `GET /ring?key=tenant:acme` | Resolve shard for a key |
| `POST /batch` | Body: `{"statements":[{"sql":"…","shard_key":"…"}]}` |

### Annotated SQL batches

`quasar/examples/batch.example.sql` shows directives:

```sql
-- @broadcast
CREATE TABLE IF NOT EXISTS cluster_meta (k TEXT, v TEXT);
GO
-- @shard tenant:acme
INSERT INTO cluster_meta VALUES ('tenant', 'acme');
```

Run with `python -m quasar batch my-cluster/cluster.json script.sql`.

### Drift detection

```bash
# File-level: empty/new shards differ by hash
python -m quasar drift ./cluster/cluster.json

# Application probe: same SQL must return identical stdout on every shard
python -m quasar drift ./cluster/cluster.json \
  --probe-sql "SELECT COUNT(*) FROM cluster_meta;"
```

### Watch loop (cron or sidecar)

```bash
# Health every 60s; full backup every tick; run forever
python -m quasar watch ./cluster/cluster.json --interval 3600 --backup

# One-shot check + incremental WAL backup (cron-friendly)
python -m quasar watch ./cluster/cluster.json --iterations 1 --backup --incremental
```

## High-throughput pool (better process-per-query)

By default, `QuasarCluster` uses **`PooledAstralDBClient`** (`pool.enabled: true` in `cluster.json`):

| Setting | Default | Effect |
|---------|---------|--------|
| `batch_window_ms` | `3` | Coalesce queries to the same `.db` within this window |
| `batch_max_statements` | `48` | Max statements per subprocess |
| `max_inflight_batches` | `24` | Cap concurrent `astraldb` processes cluster-wide |
| `warm_on_start` | `true` | `SELECT 1` on each shard at startup |

Instead of **one OS process per query**, Quasar merges many statements into a single:

```sql
BEGIN;
…statement 1…
…statement 2…
COMMIT;
```

Use `immediate=True` on `client.query()` (or cross-shard transactions) when you cannot wait for the batch window.

```bash
python -m quasar pool-stats ./cluster/cluster.json
python -m quasar workload-stats ./cluster/cluster.json
```

Disable pooling for debugging: `"pool": { "enabled": false }`.

## Cross-shard transactions

Two-phase style orchestration across shard files:

```bash
python -m quasar xtxn ./cluster/cluster.json --spec quasar/examples/xtxn.example.json
```

```json
{
  "statements": [
    { "sql": "INSERT INTO orders VALUES (1);", "shard_key": "user:10" },
    { "sql": "INSERT INTO audit VALUES ('ok');", "shard_key": "tenant:acme" }
  ]
}
```

Python:

```python
with cluster.cross_shard_transaction() as txn:
    txn.execute("INSERT …", shard_key="user:1")
    txn.execute("INSERT …", shard_key="order:9")
```

**Caveat:** not true 2PC — a crash between shard commits can leave partial state. Use for best-effort coordination only.

## Multi-region

Merge `quasar/examples/regions.example.json` into your cluster config. Each region has its own copy of the shard map (different paths). Writes hit the `write_primary` region, then **async SQL replay** to other regions.

```bash
python -m quasar region ./cluster/cluster.json "INSERT INTO t VALUES (1);" --shard-key user:1
python -m quasar region ./cluster/cluster.json "SELECT COUNT(*) FROM t;" --shard-key user:1 --read
python -m quasar region ./cluster/cluster.json --health --shard-key x
```

Reads prefer the lowest `priority` region (optionally bias with `latency_bias_ms`).

## Python API

```python
from quasar import AstralDBClient, QuasarCluster, QuasarShard, ShardNode

client = AstralDBClient()  # uses QUASAR_ASTRALDB or repo build paths
client.query("SELECT 1;", database="data/shard0.db")

cluster = QuasarCluster.from_file("my-cluster/cluster.json")
cluster.execute("INSERT INTO t VALUES (1);", shard_key="order:99")
cluster.backup_all()
print(cluster.status(hash_files=True))
print(cluster.check_drift(probe_sql="SELECT 1;"))
```

```python
from quasar import QuasarSqlBatch, QuasarDrift, QuasarRolling, shard_ring_map

print(shard_ring_map(cluster.shard, ["user:1", "user:2", "user:999"]))
QuasarSqlBatch(cluster.shard).run_file(Path("migrate.sql"))
QuasarRolling(cluster.shard).execute("ALTER TABLE t ADD COLUMN x INT;", delay_sec=2.0)
```

## Multi-master

Configure peer writers per logical shard:

```json
"multi_master": {
  "shard0": {
    "writers": ["data/shard0_a.db", "data/shard0_b.db"],
    "quorum": 2
  }
}
```

Writes with `--multi-master` or `cluster.execute(..., multi_master=True)` run on **every** writer; the call succeeds when at least `quorum` nodes ack. Reads use the first healthy writer.

## Automatic failover

```json
"failover": {
  "enabled": true,
  "auto_promote": true,
  "state_file": ".quasar/failover.json",
  "shards": {
    "shard0": {
      "primary": "data/shard0.db",
      "standbys": ["data/shard0_standby.db"]
    }
  }
}
```

```bash
python -m quasar failover ./cluster/cluster.json status
python -m quasar failover ./cluster/cluster.json run   # promote standby if primary down
```

`watch` can call `failover run` each tick when failover is enabled (integrate in your cron/sidecar). Routing state is persisted so `active_shard()` resolves the promoted path.

## Automatic rebalancing

When you **add or remove** shards, keys move on the consistent hash ring. Quasar scans exported bundle JSON, plans row moves by `rebalance.shard_key_column`, and applies `INSERT` on target shards:

```bash
# Plan only (prints JSON)
python -m quasar rebalance ./cluster/cluster.json shard0 shard1 shard2 shard3

# Plan + apply
python -m quasar rebalance ./cluster/cluster.json shard0 shard1 shard2 shard3 --apply

# Optional: remove moved rows from source
python -m quasar rebalance ./cluster/cluster.json shard0 shard1 shard2 shard3 --apply --delete-source
```

**Caveat:** Large tables are exported per shard; rebalance is batch/offline oriented, not live migration.

## Cross-shard JOIN

Quasar does **not** push JOINs into AstralDB’s planner. It exports shard data (JSON bundle), collects rows, and hash-joins in Python. Spec: `quasar/examples/cross_join.example.json`.

```bash
python -m quasar cross-join ./cluster/cluster.json quasar/examples/cross_join.example.json -o /tmp/joined.json
```

Each table entry runs on **all shards** (or one shard if `shard_key` is set). Join keys must align across shards. For analytics at scale, prefer ETL into a single AstralDB file or DuckDB.

## Sharding model

Quasar uses **application-level sharding**:

1. Your app chooses a **shard key** (user id, tenant id, hash bucket, etc.).
2. Quasar maps the key to a shard via **consistent hashing** (128 virtual nodes per physical shard by default).
3. SQL runs on that shard’s `--database` file only.

Quasar does **not**:

- Split one SQL statement across shards automatically (no distributed `JOIN`).
- Merge `SELECT` results from a fan-out into one result set.
- Provide cross-shard transactions.

For cross-shard analytics, export bundles or ETL into a single AstralDB file / external engine.

## Replication model

`QuasarReplica` runs the **same SQL string** on the master file and each replica file. This is **logical replication by replay**, not streaming WAL shipping between hosts.

- Replicas are separate files (often on the same machine or NFS).
- A failed replica write after a successful master write leaves nodes **out of sync** — your ops playbooks must handle repair (re-copy, `migrate`, or re-run DDL).
- Reads can target a random replica (`read(prefer_replica=True)`).

## Backup & restore

Full backup copies:

- Main database file (`*.db`)
- WAL (`<db>.wal`) when present
- `astraldb_procs.json` and `astraldb_procs_cache/` in the same directory

Each backup is a timestamped folder under `backup_dir` with `manifest.json`.

Incremental backup copies **WAL only** (for point-in-time style recovery between full snapshots).

`backup_retention` in cluster config triggers prune after `backup-all`.

**Caveat:** Restoring over a live database while AstralDB has it open can corrupt data. Stop writers before restore.

## Migration

Two paths:

| Method | When to use |
|--------|-------------|
| `migrate_via_bundle` (default) | Logical export/import via AstralDB’s bundle format |
| `migrate_via_checkpoint_copy` (`--copy`) | Same host, cold copy after `BEGIN; COMMIT;` checkpoint |

Bundle migration is safer across versions if bundle format is stable; file copy is faster for large local files.

## Production & security (v1.0)

Quasar **v1.0** is the production-hardened release: input validation, path containment, bounded pools, and safe HTTP defaults.

### `cluster.json` → `security`

| Key | Default | Purpose |
|-----|---------|---------|
| `max_sql_bytes` | 524288 | Reject oversized SQL before subprocess |
| `max_config_bytes` | 2097152 | Cap JSON config / join spec file size |
| `max_shard_key_bytes` | 4096 | Limit routing key length |
| `max_pool_queue` | (clamped with pool) | Backpressure when pool queue is full |
| `max_gateway_body_bytes` | 1048576 | Flask `MAX_CONTENT_LENGTH` |
| `gateway_rate_per_minute` | 1200 | Per-IP rate limit on gateway |
| `require_gateway_auth` | false | Fail gateway startup without API key |
| `redact_secrets_in_errors` | true | Hide `-P` passwords in error messages |
| `allow_path_outside_config_root` | false | Block `../` path traversal in config |

Env overrides: `QUASAR_MAX_SQL_BYTES`, `QUASAR_REQUIRE_GATEWAY_AUTH`.

### Hardening checklist

1. **Gateway** — Set `QUASAR_API_KEY` (or `--api-key`) and `--require-auth` / `require_gateway_auth: true` before exposing beyond localhost. API keys are compared with **constant-time** equality.
2. **Paths** — Keep `cluster.json`, shards, backups, and cross-join specs under one config directory; leave `allow_path_outside_config_root` false unless you operate absolute paths deliberately.
3. **SQL** — Quasar validates size and rejects null bytes; it does **not** parse SQL — untrusted SQL is still dangerous. Use app-layer parameterization and least-privilege schemas.
4. **Pool** — Tune `pool.max_queue` and `max_workers`; full queue returns **503 overloaded** instead of unbounded memory growth.
5. **Circuit breaker** — `workload.circuit_*` stops hammering failed shards (`QuasarCircuitOpenError`).
6. **Subprocess** — `shell=False`, executable validation, configurable timeouts on `AstralDBClient`.
7. **Shutdown** — Long-running commands should call `cluster.close()` to drain the pool (gateway does this on exit).

### HTTP gateway caveats

- **Not a replacement** for a production-grade SQL proxy: one request → one subprocess per shard touched (batched when pooling is enabled).
- **No connection pooling** inside AstralDB (each CLI invocation opens the DB).
- **No prepared statements** or wire protocol compatibility with PostgreSQL/MySQL.
- Fan-out queries return an **array** of per-shard results; clients must merge.
- Generic **500** responses hide internal details; security violations return **400 bad request**.

## Operational caveats (read this)

1. **Experimental stack** — AstralDB is under active development; Quasar adds another moving part. Audit before production.
2. **Batched subprocesses** — Pooling dramatically cuts process spawn overhead; cross-shard transactions and `immediate=True` still use one process per call. Extreme QPS may require a single shard or external pooler.
3. **No distributed consensus** — Split-brain, partial writes, and replica lag are your responsibility.
4. **File locking** — Only one AstralDB process should write a given database path at a time.
5. **Security** — Passwords via `-P` or env; gateway API key is a simple shared secret, not OAuth.
6. **Windows paths** — Use forward slashes in JSON or escaped backslashes; prefer `init` scaffolding.
7. **CI** — `pytest quasar/tests` runs on every CI matrix job (mock CLI + real backup I/O).

## Tests

```bash
pip install -r quasar/requirements.txt
python -m pytest quasar/tests -q
```

## See also

- [`docs/Usage.md`](Usage.md) — AstralDB CLI flags Quasar delegates to
- [`docs/Overview.md`](Overview.md) — Engine capabilities and limits
- [`README.md`](../README.md) — Build and benchmark overview
