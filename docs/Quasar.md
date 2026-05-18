# Quasar (v1.0)

**Quasar** is AstralDB’s Python orchestration layer: a small toolkit (~2 MB with optional Flask) that wraps the **`astraldb` executable** so you can run sharded clusters, replicas, backups, and migrations without building a custom server.

AstralDB itself stays a **single static binary** with no network listener. Quasar does not change the engine; it **spawns the CLI** with `--database`, `-q`, `-s`, and bundle export/import flags, then coordinates multiple database files on disk.

## Quick start

```bash
# Build AstralDB first (see README)
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release
cmake --build build-ci

pip install -e ".[dev]"
export QUASAR_ASTRALDB=build-ci/astraldb   # or astraldb.exe on Windows

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
| `merge_routed_results` / `execute_merged` | Combine broadcast `SELECT` stdout from all shards |
| `DatabaseLock` / `lock_databases` | Advisory per-`.db` locks when `locks.enabled` |
| `PooledAstralDBClient` | Batched subprocesses + worker pool (default when `pool.enabled`) |
| `RetryPolicy` / `execute_with_retry` | Transient error retries with backoff |
| `MvccConfig` | AstralDB snapshot reads (`BEGIN` file copy) for consistent SELECTs |
| `DistributedTransactionCoordinator` | Coordinator 2PC + durable WAL (`distributed_txn`) |
| `CrossShardTransaction` | Legacy interactive txn API (uses coordinator when enabled) |
| `QuasarMultiRegion` | Primary region writes + async geo-replication |
| `WorkloadGuard` | Per-shard circuit breakers under load |
| `QuasarSaga` / `QuasarLedger` | Compensating sagas and cross-shard transfers (financial profile) |
| `IdempotencyStore` | Dedup payment retries (`financial.idempotency`) |
| `QuasarJournal` | Append-only orchestration audit JSONL |
| `CrashRecoveryManager` | Automated dtxn recover + rollbacks + pool heal |
| `QuasarCrossQuery` | Parallel fan-out and scatter-gather reads |
| `QuasarAutoscale` | Metric-driven shard scale-out (and scale-in recommendations) |
| `ClusterConsensus` | Raft-style quorum for rebalance / commit decisions |
| `QuasarRebalanceAutomator` | Automatic plan/apply when the hash ring changes |
| Gateway (optional) | Flask `/query`, `/transfer`, `/recover`, `/cross-query`, `/autoscale`, `/consensus`, … |

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
| `QUASAR_ASTRALDB` / `ASTRALDB_BIN` | Path to `astraldb` |
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
| `shard CONFIG "SQL" [--shard-key K] [--merge]` | Routed or broadcast SQL (`--merge` for read fan-out) |
| `script FILE [--database DB \| --config CONFIG] [--shard-key K]` | Run `.sql` file |
| `replicate MASTER [REPLICAS...] "SQL" [--parallel]` | Write to master + replicas |
| `backup DB BACKUP_DIR [--version LABEL] [--incremental]` | Full or WAL-only backup |
| `backup-all CONFIG [--incremental] [--prefix P]` | Backup every shard |
| `backup-list BACKUP_DIR [--json]` | List versions |
| `backup-prune BACKUP_DIR [--keep-last N] [--keep-days D] [--dry-run]` | Retention |
| `restore BACKUP_DIR VERSION TARGET_DB [--overwrite] [--force]` | Restore files (blocks if WAL recently touched unless `--force`) |
| `migrate SOURCE TARGET [--copy] [--work-dir DIR]` | Bundle or file copy |
| `gateway CONFIG [--host H] [--port P] [--api-key K]` | HTTP front door |
| `status CONFIG [--hash]` | Health + disk inventory JSON |
| `drift CONFIG [--probe-sql SQL] [--no-hash] [--replicas]` | Shard (+ optional replica) consistency (`exit 1` if drift) |
| `ring CONFIG KEY [KEY...]` | Show which shard owns each key |
| `rolling CONFIG "SQL" [--delay SEC]` | Sequential shard execution |
| `batch CONFIG SCRIPT.sql` | Annotated multi-shard SQL file |
| `watch CONFIG [--interval SEC] [--backup] [--drift] [--recover] [--probe-sql SQL] [--replicas] [--iterations N]` | Health/backup/drift/recovery loop |
| `recover CONFIG [--force]` | One-shot crash recovery (dtxn + rollbacks + pool heal) |
| `cross-query CONFIG "SQL" [--merge] [--spec FILE] [--no-parallel]` | Fan-out or scatter-gather across shards |
| `repair-replica CONFIG REPLICA_SET` | Recopy master → slaves from config |
| `compile SQL.sql [-o out.abc] [--pool]` | Wrapper for `astraldb -cc` |
| `multi-master CONFIG "SQL" --shard-key K [--read]` | Quorum write or read from master group |
| `failover CONFIG {status\|run\|reset\|fail-back SHARD}` | Failover state, promotion, fail-back |
| `rebalance CONFIG SHARD... [--apply] [--plan-file F]` | Plan/apply automatic rebalancing |
| `cross-join CONFIG spec.json [-o out.json]` | Cross-shard JOIN |
| `pool-stats CONFIG` | Batching / pool counters |
| `workload-stats CONFIG` | Circuit breaker state |
| `xtxn CONFIG --spec FILE` | Cross-shard ACID transaction (coordinator 2PC) |
| `dtxn-recover CONFIG` | Abort in-doubt transactions from coordinator WAL |
| `region CONFIG "SQL" --shard-key K [--read] [--region NAME]` | Multi-region I/O |
| `saga CONFIG spec.json` | Compensating saga (`exit 1` unless completed) |
| `transfer CONFIG spec.json` | Cross-shard ledger transfer via saga |
| `reconcile CONFIG [--probe-sql SQL]` | Compare balances or probe across shards |
| `financial-init CONFIG` | Bootstrap `accounts` / `ledger_entries` on all shards |
| `journal CONFIG [--limit N]` | Tail Quasar operation journal |
| `recover CONFIG [--force]` | Crash recovery pipeline |
| `cross-query CONFIG "SQL" [--merge] [--spec FILE]` | Cross-shard fan-out / scatter-gather |
| `autoscale CONFIG [--apply] [--metrics]` | Evaluate or apply shard autoscaling |
| `consensus CONFIG [--elect]` | Consensus leader / quorum status |
| `rebalance-auto CONFIG [--apply]` | Automatic rebalance for current shard ring |

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
| `GET /status?hash=1` | Health + disk inventory |
| `GET /drift?probe_sql=…&replicas=1` | Drift report (`409` if inconsistent) |
| `POST /query` with `"merge": true` | Merged broadcast read stdout (read SQL only) |
| `POST /transfer` | Ledger transfer spec JSON |
| `POST /saga` | Saga spec JSON |
| `GET /reconcile` | Balance / probe reconciliation (`409` if inconsistent) |

### Fan-out result merge

Broadcast reads return one combined stdout stream instead of per-shard arrays:

```bash
python -m quasar shard my-cluster/cluster.json "SELECT COUNT(*) FROM meta;" --merge
```

Python:

```python
merged = cluster.execute_merged("SELECT k, v FROM meta;")
print(merged.merged_stdout)
```

Gateway: `POST /query` with `{"sql":"SELECT 1","merge":true}`.

### Advisory locks

Enable in `cluster.json` to reduce concurrent-writer corruption risk:

```json
"locks": { "enabled": true, "timeout_sec": 30 }
```

Quasar acquires an exclusive lock file under `.quasar/locks/` before routed writes.

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
| `rollback_on_batch_failure` | `true` | Issue `ROLLBACK` if a batched write fails |
| `read_lane_immediate` | `true` | Route reads outside the write batch queue |
| `per_db_max_inflight` | `2` | Cap concurrent subprocesses per `.db` file |
| `keepalive_interval_sec` | `0` | Background `SELECT 1` on warmed databases (`0` = off) |

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

**Session pooling** — Quasar does not hold sockets inside AstralDB; pooling means batched subprocess lanes, per-database concurrency caps, optional keepalive pings on warmed shards, and `heal()` after crashes (drain pending queue, `ROLLBACK`, re-probe).

## MVCC snapshot reads

AstralDB **`BEGIN`** copies the main database file to a sidecar snapshot (`.txn.snap`); statements in that subprocess see one stable view until **`COMMIT`** or **`ROLLBACK`**. Quasar uses this for routed reads under load:

```json
"mvcc": {
  "snapshot_reads": true,
  "read_immediate": true,
  "temporal_as_of": null
}
```

With `snapshot_reads`, read SQL is sent as `BEGIN; …; COMMIT;` on an **immediate** subprocess (not mixed into write batches). Set `temporal_as_of` to append `FOR SYSTEM TIME AS OF` on `SELECT` statements (AstralDB temporal tables).

```python
client.query_snapshot("SELECT COUNT(*) FROM orders;", database="data/shard0.db")
```

## Error recovery and workload

```json
"recovery": {
  "max_retries": 3,
  "base_delay_ms": 50,
  "max_delay_ms": 2000,
  "jitter": true
},
"workload": {
  "circuit_failure_threshold": 5,
  "circuit_cooldown_sec": 30,
  "circuit_half_open_max_probes": 1
}
```

| Mechanism | Behavior |
|-----------|----------|
| **Retries** | Transient failures (timeouts, pool overload, I/O blips) retry with exponential backoff on routed queries and pool batches |
| **Circuit breaker** | After repeated shard failures, requests fail fast; after cooldown, **half-open** probes allow a trial request |
| **Batch rollback** | Failed write batches trigger `ROLLBACK` on that database file |
| **Gateway** | `503` responses include `Retry-After` for overload and open circuits |

`workload-stats` and `pool-stats` include retry counters (`retry_*`).

## Automated crash recovery

On cluster startup (and optionally on a timer), Quasar can run a coordinated recovery pipeline:

```json
"recovery_automation": {
  "enabled": true,
  "on_start": true,
  "rollback_orphan_txns": true,
  "recover_dtxn": true,
  "heal_pool": true,
  "interval_sec": 0
}
```

| Step | Action |
|------|--------|
| **dtxn** | Abort in-doubt coordinator transactions from `.quasar/dtxn/wal.jsonl` |
| **rollback** | `ROLLBACK` on every active shard database |
| **pool_heal** | Drain batch queue, rollback warmed DBs, `SELECT 1` probe |

```bash
python -m quasar recover ./cluster/cluster.json
python -m quasar watch ./cluster/cluster.json --recover --interval 60
curl -X POST http://127.0.0.1:8080/recover -H "X-Quasar-Key: $QUASAR_API_KEY"
```

Set `interval_sec` > 0 for a background recovery loop (daemon thread). Financial workloads log `CRASH_RECOVERY` to the Quasar journal when enabled.

## Cross-shard queries

Beyond `shard --merge` and cross-join bundles, **`cross-query`** provides fan-out and scatter-gather helpers:

```bash
# Same SQL on every shard (parallel by default)
python -m quasar cross-query ./cluster/cluster.json "SELECT COUNT(*) FROM orders;"

# Heterogeneous SQL routed by shard key
python -m quasar cross-query ./cluster/cluster.json --spec queries.json
```

`queries.json`:

```json
{
  "queries": [
    { "sql": "SELECT balance FROM accounts WHERE id = 1;", "shard_key": "acct:1" },
    { "sql": "SELECT balance FROM accounts WHERE id = 2;", "shard_key": "acct:2" }
  ]
}
```

Gateway: `POST /cross-query` with body `{ "sql": "…", "merge": false }` or `{ "queries": […] }`. Responses include per-shard stdout, optional merged read output, and parsed row tags (`_shard`).

Enable/disable via `"cross_query": { "enabled": true }` in `cluster.json`.

## Autoscaling

Quasar **v1.0** can recommend or apply **horizontal shard scaling** from in-process metrics (pool rejections, monitor latency/error rate, open circuits, per-shard disk size). It does not provision VMs or Kubernetes pods—it adds new `data/shardN.db` files and updates `cluster.json`, then you **rebalance** row data onto the new ring.

```json
"autoscaling": {
  "enabled": true,
  "min_shards": 3,
  "max_shards": 12,
  "scale_out_step": 1,
  "cooldown_sec": 600,
  "auto_apply": false,
  "rebalance_on_scale": true,
  "scale_out_pool_rejects": 5,
  "scale_out_error_rate": 0.1,
  "scale_out_mean_latency_ms": 500,
  "scale_out_max_shard_bytes": 5368709120,
  "scale_in_error_rate": 0.01,
  "scale_in_mean_latency_ms": 80,
  "scale_in_max_shard_bytes": 1073741824
}
```

| Signal | Scale out when | Scale in when (advisory) |
|--------|----------------|---------------------------|
| Pool | `rejected_queue_full` ≥ threshold | No rejections, circuits closed |
| Monitor | `error_rate` or `mean_latency_ms` high | Low error rate and latency |
| Disk | Any shard ≥ `scale_out_max_shard_bytes` | All shards below `scale_in_max_shard_bytes` |
| Circuits | Any shard circuit open | — |

```bash
python -m quasar autoscale ./cluster/cluster.json
python -m quasar autoscale ./cluster/cluster.json --apply
python -m quasar autoscale ./cluster/cluster.json --metrics
python -m quasar watch ./cluster/cluster.json --autoscale --autoscale-apply --interval 120
curl http://127.0.0.1:8080/autoscale -H "X-Quasar-Key: $QUASAR_API_KEY"
curl -X POST http://127.0.0.1:8080/autoscale -H "Content-Type: application/json" \
  -d '{"apply": true}'
```

**Scale-out** (`--apply` or `auto_apply: true`) appends shards, warms them with `SELECT 1`, and optionally emits a **rebalance plan** (`rebalance_on_scale`). **Scale-in** never deletes data—it returns drain/rebalance steps for the last shard.

State and cooldown: `.quasar/autoscale.json`. Example profile: `quasar/examples/autoscale.cluster.example.json`.

## Financial and banking workloads

Quasar can orchestrate **ledger-style transfers**, **idempotent payments**, and **compensating sagas** across shards. This is an **example profile** for banking/fintech patterns—not a certified payment switch. AstralDB remains experimental; use external settlement and compliance review for production money movement.

Enable in `cluster.json` (see `quasar/examples/banking.cluster.example.json`):

```json
"financial": {
  "enabled": true,
  "currency": "USD",
  "journal_file": ".quasar/journal.jsonl",
  "idempotency_dir": ".quasar/idempotency",
  "saga_state_dir": ".quasar/sagas",
  "idempotency": { "store": "file", "require_key": true }
}
```

| Feature | Purpose |
|---------|---------|
| **Saga** | Ordered forward steps with `compensate_sql` on failure (reverse order) |
| **Transfer** | Debit/credit across shard keys using integer **cents** (no floats) |
| **Idempotency** | File-backed (default) or AstralDB table; safe client retries |
| **Journal** | Quasar-level JSONL audit of transfers, sagas, idempotent replays |
| **Reconcile** | `SUM(balance_cents)` or custom probe compared on every shard |
| **MVCC reads** | Balance inquiries via snapshot `BEGIN` (consistent per subprocess) |
| **Strict region sync** | `region_write_strict()` waits for all regions (set `async_replicate: false`) |

```bash
python -m quasar financial-init ./bank/cluster.json
python -m quasar transfer ./bank/cluster.json quasar/examples/transfer.example.json
python -m quasar saga ./bank/cluster.json quasar/examples/saga.example.json
python -m quasar reconcile ./bank/cluster.json
python -m quasar journal ./bank/cluster.json --limit 20
```

Gateway: `POST /transfer`, `POST /saga`, `GET /reconcile` with JSON bodies matching the example specs.

Python:

```python
cluster.execute_idempotent(
    "INSERT INTO payments …",
    shard_key="acct:1001",
    idempotency_key="pay-uuid-…",
)
cluster.transfer({...})
cluster.run_saga({"saga_id": "…", "steps": [...]})
```

Pair with AstralDB **`--audit-file`** per shard (RBAC/login events) and Quasar **`journal`** for orchestration. Amounts are **integer cents**; routing uses **account id → shard_key** (e.g. `acct:1001`).

**Caveats:** cross-shard money movement defaults to **saga + compensation**; enable **`distributed_txn` + `consensus`** for strict 2PC ACID instead. No built-in PCI/HSM; regulatory controls are your responsibility.

## Distributed consensus

Orchestration decisions (rebalance apply, commit votes) can require a **majority quorum** before execution. State lives under `.quasar/consensus/` (log + per-member votes). On one host all members vote locally; for multiple machines replicate `state_dir` or use a shared volume.

```json
"consensus": {
  "enabled": true,
  "members": ["shard0", "shard1", "shard2"],
  "self_id": "shard0",
  "state_dir": ".quasar/consensus",
  "require_for_rebalance": true,
  "require_for_dtxn_commit": true
}
```

```bash
python -m quasar consensus ./cluster/cluster.json
python -m quasar consensus ./cluster/cluster.json --elect
curl http://127.0.0.1:8080/consensus
```

Only the **leader** (`self_id`) may propose; quorum is `(N/2)+1`. Example: `quasar/examples/consensus.cluster.example.json`.

## Distributed ACID (strict 2PC)

**True cross-shard ACID** uses strict **two-phase commit** with **dual durable logs**:

| Log | Path | Role |
|-----|------|------|
| Coordinator WAL | `.quasar/dtxn/wal.jsonl` | Global decision (`decision_committed` before `COMMIT`) |
| Participant log | `.quasar/dtxn/participants/{shard}.jsonl` | Per-shard `prepared` / `aborted` votes |

```json
"distributed_txn": {
  "enabled": true,
  "wal_file": ".quasar/dtxn/wal.jsonl",
  "participant_log_dir": ".quasar/dtxn/participants",
  "strict_2pc": true,
  "require_consensus_for_commit": true,
  "recover_on_start": true
}
```

Protocol per `xid`:

1. `started` → `BEGIN` + statements on each participant  
2. Participant logs `prepared` → coordinator logs `prepared`  
3. Optional **consensus** quorum on `dtxn_commit`  
4. Coordinator logs **`decision_committed`** (durable) **before** sending `COMMIT`  
5. `committing` → `COMMIT` all → `committed`  

Recovery: if `decision_committed` is in the WAL, **`dtxn-recover` completes commits**; if only `prepared`, participants are aborted.

```bash
python -m quasar xtxn ./cluster/cluster.json --spec quasar/examples/xtxn.example.json
python -m quasar dtxn-recover ./cluster/cluster.json
```

```json
{
  "statements": [
    { "sql": "INSERT INTO orders VALUES (1);", "shard_key": "user:10" },
    { "sql": "INSERT INTO audit VALUES ('ok');", "shard_key": "tenant:acme" }
  ]
}
```

```python
result = cluster.cross_shard_transaction([
    {"sql": "INSERT …", "shard_key": "user:1"},
    {"sql": "INSERT …", "shard_key": "order:9"},
])
assert result.committed and result.acid
```

**Limits:** ACID is across **Quasar-coordinated AstralDB shard files**, not external databases. Pair with **sagas** when you need compensating business logic beyond rollback.

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
  "failure_threshold": 3,
  "cooldown_after_promote_sec": 30,
  "fail_back_to_primary": true,
  "health_probe_sql": "SELECT 1;",
  "shards": {
    "shard0": {
      "primary": "data/shard0.db",
      "standbys": ["data/shard0_standby.db"]
    }
  }
}
```

| Setting | Effect |
|---------|--------|
| `failure_threshold` | Consecutive failed probes before promotion (anti-flap) |
| `cooldown_after_promote_sec` | Minimum time between promotions |
| `fail_back_to_primary` | Route back to config primary when it becomes healthy |
| `health_probe_sql` | SQL used for health (default `SELECT 1;`) |

```bash
python -m quasar failover ./cluster/cluster.json status
python -m quasar failover ./cluster/cluster.json run
python -m quasar failover ./cluster/cluster.json fail-back shard0
```

`watch --failover` runs a failover tick each interval. `active_shard()` resolves the promoted path from persisted state.

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

### Automatic rebalancing

When the shard list or hash ring changes (including after **autoscale**), Quasar can plan and apply row moves on a schedule:

```json
"rebalance": {
  "shard_key_column": "id",
  "automation": {
    "enabled": true,
    "auto_apply": false,
    "interval_sec": 0,
    "delete_from_source": true,
    "require_consensus": true,
    "max_moves_per_tick": 5000
  }
}
```

```bash
python -m quasar rebalance-auto ./cluster/cluster.json
python -m quasar rebalance-auto ./cluster/cluster.json --apply
python -m quasar watch ./cluster/cluster.json --rebalance-auto --rebalance-apply
curl -X POST http://127.0.0.1:8080/rebalance-auto -d '{"apply": true}'
```

With `autoscaling.rebalance_on_scale: true`, scale-out can chain into automatic rebalance. Consensus quorum is required when `require_consensus` is set.

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
- Provide cross-shard transactions without `distributed_txn` (use coordinator 2PC for ACID).

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

**Caveat:** Restoring over a live database while AstralDB has it open can corrupt data. By default, restore refuses if the target WAL was modified within the last 2 seconds; use `--force` after stopping writers.

## Migration

Two paths:

| Method | When to use |
|--------|-------------|
| `migrate_via_bundle` (default) | Logical export/import via AstralDB’s bundle format |
| `migrate_via_checkpoint_copy` (`--copy`) | Same host, cold copy after `BEGIN; COMMIT;` checkpoint |

Bundle migration is safer across versions if bundle format is stable; file copy is faster for large local files.

## v1.0 operations reference

Single-page map of production-oriented features (all **v1.0.0**—no separate “enhanced” release).

| Area | Config key | CLI | Gateway |
|------|------------|-----|---------|
| **Pooling** | `pool.*` | `pool-stats` | `/metrics` |
| **MVCC reads** | `mvcc.*` | (via routed queries) | `/query` |
| **Retries / circuit** | `recovery`, `workload` | `workload-stats` | `/metrics` |
| **Crash recovery** | `recovery_automation` | `recover`, `watch --recover` | `POST /recover` |
| **Cross-shard reads** | `cross_query` | `cross-query`, `shard --merge` | `POST /cross-query` |
| **Consensus** | `consensus` | `consensus`, `consensus --elect` | `GET/POST /consensus` |
| **Distributed ACID** | `distributed_txn` | `xtxn`, `dtxn-recover` | `POST /dtxn/recover` |
| **Failover** | `failover` | `failover run`, `fail-back` | `POST /failover/run` |
| **Rebalance** | `rebalance` | `rebalance … --apply` | — |
| **Auto rebalance** | `rebalance.automation` | `rebalance-auto`, `watch --rebalance-auto` | `POST /rebalance-auto` |
| **Autoscale** | `autoscaling` | `autoscale`, `watch --autoscale` | `GET/POST /autoscale` |
| **Drift / locks** | `locks` | `drift`, (locks internal) | `GET /drift` |
| **Financial** | `financial` | `transfer`, `saga`, `reconcile` | `/transfer`, `/saga` |
| **Watch loop** | — | `watch` | — |

Typical cron / sidecar stack:

```bash
python -m quasar watch ./cluster/cluster.json \
  --interval 60 --backup --incremental --failover --drift --recover --autoscale --rebalance-auto
```

Tune `autoscaling.auto_apply` only when rebalance playbooks are automated; otherwise evaluate with `autoscale` and apply scale-out manually before `rebalance --apply`.

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
5. **Circuit breaker** — `workload.circuit_*` stops hammering failed shards (`QuasarCircuitOpenError`); tune `recovery.max_retries` for transient AstralDB errors.
6. **MVCC** — Snapshot reads add a `BEGIN`/`COMMIT` round trip per read; disable `mvcc.snapshot_reads` if you accept read-your-writes from the latest committed state only.
7. **Subprocess** — `shell=False`, executable validation, configurable timeouts on `AstralDBClient`.
8. **Shutdown** — Long-running commands should call `cluster.close()` to drain the pool (gateway does this on exit).

### HTTP gateway caveats

- **Not a replacement** for a production-grade SQL proxy: one request → one subprocess per shard touched (batched when pooling is enabled).
- **No in-process SQL connections** inside AstralDB (each CLI invocation opens the DB); Quasar pools **subprocess lanes** and caps per-database concurrency instead.
- **No prepared statements** or wire protocol compatibility with PostgreSQL/MySQL.
- Fan-out queries return an **array** of per-shard results by default; use `--merge` / `execute_merged` / `"merge":true` for broadcast reads.
- Generic **500** responses hide internal details; security violations return **400 bad request**.

## Operational caveats (read this!!!)

1. **Experimental stack** — AstralDB is under active development; Quasar adds another moving part. Audit before production.
2. **Batched subprocesses** — Pooling dramatically cuts process spawn overhead; cross-shard transactions and `immediate=True` still use one process per call. Extreme QPS may require a single shard or external pooler.
3. **Consensus scope** — File-quorum consensus coordinates **orchestration** (rebalance, commit decisions), not AstralDB storage engine replication. Split-brain across DB files still requires failover/replica playbooks.
4. **File locking** — Only one AstralDB process should write a given database path at a time.
5. **Security** — Passwords via `-P` or env; gateway API key is a simple shared secret, not OAuth.
6. **Windows paths** — Prefer `python -m quasar init` (writes `data/shard0.db` style paths). Hand-edited `cluster.json` may use forward slashes; Quasar normalizes them on load. Avoid raw backslashes in JSON unless escaped (`\\`). `QUASAR_ASTRALDB` should point at `astraldb.exe` (see README).
7. **CI** — `pytest quasar/tests` runs on every CI matrix job (mock CLI + real backup I/O).

## Tests

```bash
pip install -e ".[dev]"
python -m pytest quasar/tests -q
```

## See also

- [`docs/Usage.md`](Usage.md) — AstralDB CLI flags Quasar delegates to
- [`docs/Overview.md`](Overview.md) — Engine capabilities and limits
- [`README.md`](../README.md) — Build and benchmark overview
