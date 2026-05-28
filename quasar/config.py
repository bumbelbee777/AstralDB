"""Load and validate Quasar cluster JSON configs."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional, Union

from quasar.paths import resolve_config_path
from quasar.security import (
    SecurityPolicy,
    load_json_config_file,
    validate_shard_name,
    validate_sql_identifier,
)

PathLike = Union[str, Path]


class ConfigError(ValueError):
    """Invalid or incomplete Quasar cluster configuration."""


def _require_mapping(obj: Any, label: str) -> Dict[str, Any]:
    if not isinstance(obj, dict):
        raise ConfigError(f"{label} must be a JSON object")
    return obj


def validate_cluster_config(config: Dict[str, Any], policy: Optional[SecurityPolicy] = None) -> None:
    policy = policy or SecurityPolicy()
    root = _require_mapping(config, "root")
    shards = root.get("shards")
    if not isinstance(shards, list) or not shards:
        raise ConfigError("'shards' must be a non-empty array")

    names: List[str] = []
    for i, entry in enumerate(shards):
        shard = _require_mapping(entry, f"shards[{i}]")
        for key in ("name", "database"):
            if key not in shard or not isinstance(shard[key], str) or not shard[key].strip():
                raise ConfigError(f"shards[{i}] missing non-empty '{key}'")
        try:
            validate_shard_name(shard["name"])
        except Exception as exc:
            raise ConfigError(str(exc)) from exc
        if shard["name"] in names:
            raise ConfigError(f"duplicate shard name: {shard['name']}")
        names.append(shard["name"])

    vn = root.get("virtual_nodes", 128)
    if not isinstance(vn, int) or vn < 1 or vn > 10_000:
        raise ConfigError("'virtual_nodes' must be an integer between 1 and 10000")

    replicas = root.get("replicas", {})
    if replicas is not None:
        if not isinstance(replicas, dict):
            raise ConfigError("'replicas' must be an object")
        for name, spec in replicas.items():
            rep = _require_mapping(spec, f"replicas.{name}")
            if "master" not in rep or not isinstance(rep["master"], str):
                raise ConfigError(f"replicas.{name} missing 'master'")
            slaves = rep.get("slaves", [])
            if slaves is not None and not isinstance(slaves, list):
                raise ConfigError(f"replicas.{name}.slaves must be an array")

    retention = root.get("backup_retention")
    if retention is not None:
        ret = _require_mapping(retention, "backup_retention")
        if "keep_last" in ret and (not isinstance(ret["keep_last"], int) or ret["keep_last"] < 0):
            raise ConfigError("backup_retention.keep_last must be a non-negative integer")
        if "keep_days" in ret and not isinstance(ret["keep_days"], (int, float)):
            raise ConfigError("backup_retention.keep_days must be a number")

    failover = root.get("failover")
    if failover is not None:
        fo = _require_mapping(failover, "failover")
        if "shards" in fo and not isinstance(fo["shards"], dict):
            raise ConfigError("failover.shards must be an object")

    multi_master = root.get("multi_master")
    if multi_master is not None and not isinstance(multi_master, dict):
        raise ConfigError("multi_master must be an object")

    rebalance = root.get("rebalance")
    if rebalance is not None:
        rb = _require_mapping(rebalance, "rebalance")
        if "shard_key_column" in rb:
            try:
                validate_sql_identifier(rb["shard_key_column"], label="shard_key_column")
            except Exception as exc:
                raise ConfigError(str(exc)) from exc

    regions = root.get("regions")
    if regions is not None and not isinstance(regions, dict):
        raise ConfigError("regions must be an object")

    pool = root.get("pool")
    if pool is not None and not isinstance(pool, dict):
        raise ConfigError("pool must be an object")

    locks = root.get("locks")
    if locks is not None:
        if not isinstance(locks, dict):
            raise ConfigError("locks must be an object")
        if "timeout_sec" in locks and not isinstance(locks["timeout_sec"], (int, float)):
            raise ConfigError("locks.timeout_sec must be a number")

    recovery = root.get("recovery")
    if recovery is not None and not isinstance(recovery, dict):
        raise ConfigError("recovery must be an object")

    mvcc = root.get("mvcc")
    if mvcc is not None and not isinstance(mvcc, dict):
        raise ConfigError("mvcc must be an object")

    financial = root.get("financial")
    if financial is not None and not isinstance(financial, dict):
        raise ConfigError("financial must be an object")

    dtxn = root.get("distributed_txn")
    if dtxn is not None and not isinstance(dtxn, dict):
        raise ConfigError("distributed_txn must be an object")

    recovery_auto = root.get("recovery_automation")
    if recovery_auto is not None and not isinstance(recovery_auto, dict):
        raise ConfigError("recovery_automation must be an object")

    cross_query = root.get("cross_query")
    if cross_query is not None and not isinstance(cross_query, dict):
        raise ConfigError("cross_query must be an object")

    autoscaling = root.get("autoscaling")
    if autoscaling is not None and not isinstance(autoscaling, dict):
        raise ConfigError("autoscaling must be an object")

    consensus = root.get("consensus")
    if consensus is not None and not isinstance(consensus, dict):
        raise ConfigError("consensus must be an object")

    fdw = root.get("fdw")
    if fdw is not None and not isinstance(fdw, dict):
        raise ConfigError("fdw must be an object")
    htap = root.get("htap")
    if htap is not None and not isinstance(htap, dict):
        raise ConfigError("htap must be an object")
    pitr = root.get("pitr")
    if pitr is not None and not isinstance(pitr, dict):
        raise ConfigError("pitr must be an object")
    upgrades = root.get("upgrades")
    if upgrades is not None and not isinstance(upgrades, dict):
        raise ConfigError("upgrades must be an object")
    cdc = root.get("cdc")
    if cdc is not None and not isinstance(cdc, dict):
        raise ConfigError("cdc must be an object")
    serverless = root.get("serverless")
    if serverless is not None and not isinstance(serverless, dict):
        raise ConfigError("serverless must be an object")
    split_merge = root.get("split_merge")
    if split_merge is not None and not isinstance(split_merge, dict):
        raise ConfigError("split_merge must be an object")
    edge = root.get("edge")
    if edge is not None and not isinstance(edge, dict):
        raise ConfigError("edge must be an object")
    distributed_join = root.get("distributed_join")
    if distributed_join is not None and not isinstance(distributed_join, dict):
        raise ConfigError("distributed_join must be an object")
    gsi = root.get("gsi")
    if gsi is not None and not isinstance(gsi, dict):
        raise ConfigError("gsi must be an object")
    matview = root.get("matview")
    if matview is not None and not isinstance(matview, dict):
        raise ConfigError("matview must be an object")


def normalize_cluster_config(
    config: Dict[str, Any],
    config_path: Optional[PathLike] = None,
    policy: Optional[SecurityPolicy] = None,
) -> Dict[str, Any]:
    """Return a copy with paths resolved relative to the config file directory."""
    policy = policy or SecurityPolicy.from_config(config.get("security"))
    validate_cluster_config(config, policy)
    base = Path(config_path).resolve().parent if config_path else None
    allow_outside = policy.allow_path_outside_config_root

    def _path(value: str) -> str:
        return resolve_config_path(base, value, allow_outside=allow_outside)

    out: Dict[str, Any] = dict(config)
    if "security" not in out:
        out["security"] = policy.__dict__

    out["shards"] = [
        {
            **shard,
            "database": _path(shard["database"]),
        }
        for shard in config["shards"]
    ]

    if "replicas" in config:
        replicas: Dict[str, Any] = {}
        for name, spec in config["replicas"].items():
            replicas[name] = {
                "master": _path(spec["master"]),
                "slaves": [_path(p) for p in spec.get("slaves", [])],
            }
        out["replicas"] = replicas

    if "backup_dir" in config and isinstance(config["backup_dir"], str):
        out["backup_dir"] = _path(config["backup_dir"])

    if "failover" in config:
        fo = dict(config["failover"])
        if "state_file" in fo and isinstance(fo["state_file"], str):
            fo["state_file"] = _path(fo["state_file"])
        if "shards" in fo:
            resolved_shards: Dict[str, Any] = {}
            for name, spec in fo["shards"].items():
                validate_shard_name(name)
                entry = dict(spec)
                entry["primary"] = _path(spec["primary"])
                entry["standbys"] = [_path(p) for p in spec.get("standbys", [])]
                resolved_shards[name] = entry
            fo["shards"] = resolved_shards
        out["failover"] = fo

    if "multi_master" in config:
        mm: Dict[str, Any] = {}
        for name, spec in config["multi_master"].items():
            validate_shard_name(name)
            mm[name] = {
                "writers": [_path(p) for p in spec.get("writers", [])],
                "quorum": max(1, int(spec.get("quorum", 1))),
            }
        out["multi_master"] = mm

    if "regions" in config:
        resolved_regions: Dict[str, Any] = {}
        for rname, spec in config["regions"].items():
            entry = dict(spec)
            entry["shards"] = [
                {
                    **s,
                    "database": _path(s["database"]),
                }
                for s in spec.get("shards", [])
            ]
            resolved_regions[rname] = entry
        out["regions"] = resolved_regions

    if "pool" in config and isinstance(config["pool"], dict):
        from quasar.security import clamp_pool_config

        out["pool"] = clamp_pool_config(config["pool"])

    if "pool" in out and isinstance(out["pool"], dict):
        pool = out["pool"]
        pool.setdefault("rollback_on_batch_failure", True)
        pool.setdefault("read_lane_immediate", True)
        pool.setdefault("per_db_max_inflight", 2)
        pool.setdefault("keepalive_interval_sec", 0)

    if "distributed_txn" in config:
        dtxn = dict(config["distributed_txn"])
        if "wal_file" in dtxn and isinstance(dtxn["wal_file"], str):
            dtxn["wal_file"] = _path(dtxn["wal_file"])
        if "participant_log_dir" in dtxn and isinstance(dtxn["participant_log_dir"], str):
            dtxn["participant_log_dir"] = _path(dtxn["participant_log_dir"])
        out["distributed_txn"] = dtxn

    if "consensus" in config:
        consensus = dict(config["consensus"])
        if "state_dir" in consensus and isinstance(consensus["state_dir"], str):
            consensus["state_dir"] = _path(consensus["state_dir"])
        out["consensus"] = consensus

    if "financial" in config:
        financial = dict(config["financial"])
        for key in ("journal_file", "idempotency_dir", "saga_state_dir"):
            if key in financial and isinstance(financial[key], str):
                financial[key] = _path(financial[key])
        out["financial"] = financial

    if "autoscaling" in config:
        autoscale = dict(config["autoscaling"])
        if "state_file" in autoscale and isinstance(autoscale["state_file"], str):
            autoscale["state_file"] = _path(autoscale["state_file"])
        out["autoscaling"] = autoscale

    return out


def load_cluster_config(path: PathLike) -> Dict[str, Any]:
    config_path = Path(path).resolve()
    policy = SecurityPolicy()
    raw = load_json_config_file(config_path, policy)
    policy = SecurityPolicy.from_config(raw.get("security"))
    return normalize_cluster_config(raw, config_path, policy)


def default_cluster_template(shard_count: int = 3) -> Dict[str, Any]:
    shards = [
        {"name": f"shard{i}", "database": f"data/shard{i}.db"} for i in range(shard_count)
    ]
    mm = {s["name"]: {"writers": [s["database"]], "quorum": 1} for s in shards}
    return {
        "shards": shards,
        "virtual_nodes": 128,
        "backup_dir": "backups",
        "backup_retention": {"keep_last": 14, "keep_days": 30},
        "replicas": {},
        "multi_master": mm,
        "failover": {
            "enabled": False,
            "auto_promote": True,
            "state_file": ".quasar/failover.json",
            "failure_threshold": 3,
            "cooldown_after_promote_sec": 30,
            "fail_back_to_primary": True,
            "shards": {},
        },
        "distributed_txn": {
            "enabled": True,
            "wal_file": ".quasar/dtxn/wal.jsonl",
            "participant_log_dir": ".quasar/dtxn/participants",
            "recover_on_start": True,
            "strict_2pc": True,
            "require_consensus_for_commit": False,
        },
        "consensus": {
            "enabled": False,
            "members": ["local"],
            "self_id": "local",
            "state_dir": ".quasar/consensus",
            "require_for_rebalance": True,
            "require_for_dtxn_commit": True,
        },
        "recovery_automation": {
            "enabled": True,
            "on_start": True,
            "rollback_orphan_txns": True,
            "recover_dtxn": True,
            "heal_pool": True,
            "interval_sec": 0,
        },
        "cross_query": {"enabled": True},
        "autoscaling": {
            "enabled": False,
            "min_shards": 1,
            "max_shards": 32,
            "scale_out_step": 1,
            "cooldown_sec": 300,
            "auto_apply": False,
            "rebalance_on_scale": False,
            "state_file": ".quasar/autoscale.json",
            "scale_out_pool_rejects": 1,
            "scale_out_error_rate": 0.15,
            "scale_out_mean_latency_ms": 750,
            "scale_out_max_shard_bytes": 0,
            "scale_in_error_rate": 0.02,
            "scale_in_mean_latency_ms": 100,
            "scale_in_max_shard_bytes": 0,
        },
        "rebalance": {
            "shard_key_column": "id",
            "auto_apply": False,
            "automation": {
                "enabled": False,
                "auto_apply": False,
                "interval_sec": 0,
                "delete_from_source": False,
                "require_consensus": True,
            },
        },
        "pool": {
            "enabled": True,
            "batch_max_statements": 48,
            "batch_window_ms": 3.0,
            "max_workers": 32,
            "max_inflight_batches": 24,
            "max_queue": 10000,
            "warm_on_start": True,
        },
        "regions": {},
        "regions_global": {"async_replicate": True},
        "workload": {
            "circuit_failure_threshold": 5,
            "circuit_cooldown_sec": 30,
            "circuit_half_open_max_probes": 1,
        },
        "recovery": {
            "max_retries": 3,
            "base_delay_ms": 50,
            "max_delay_ms": 2000,
            "jitter": True,
        },
        "mvcc": {
            "snapshot_reads": True,
            "read_immediate": True,
        },
        "locks": {"enabled": False, "timeout_sec": 30},
        "financial": {
            "enabled": False,
            "currency": "USD",
            "journal_file": ".quasar/journal.jsonl",
            "idempotency_dir": ".quasar/idempotency",
            "saga_state_dir": ".quasar/sagas",
            "idempotency": {"store": "file", "require_key": False},
        },
        "security": {
            "max_sql_bytes": 524288,
            "require_gateway_auth": False,
            "gateway_allow_query_api_key": False,
            "gateway_keys_file": ".quasar/gateway_keys.json",
            "redact_secrets_in_errors": True,
            "allow_path_outside_config_root": False,
        },
        "fdw": {
            "enabled": True,
            "timeout_sec": 2.0,
            "allowed_source_types": ["http_json"],
        },
        "htap": {
            "enabled": True,
            "oltp_queue_soft_limit": 0.9,
            "olap_queue_soft_limit": 0.6,
            "prefer_immediate_for_oltp": True,
        },
        "pitr": {
            "enabled": True,
            "timeline_id": "main",
            "archive_dir": ".quasar/pitr/archive",
        },
        "upgrades": {
            "enabled": True,
            "canary_shards": 1,
            "max_unhealthy_shards": 0,
        },
        "cdc": {
            "enabled": True,
            "timeline_id": "main",
            "checkpoint_file": ".quasar/cdc/checkpoints.json",
            "sink_file": ".quasar/cdc/events.jsonl",
            "dlq_file": ".quasar/cdc/dlq.jsonl",
        },
        "serverless": {
            "enabled": True,
            "state_file": ".quasar/serverless/lease.json",
            "idle_sec": 30,
        },
        "split_merge": {
            "enabled": True,
            "journal_file": ".quasar/split_merge/journal.json",
        },
        "edge": {
            "enabled": True,
            "registry_file": ".quasar/edge/registry.json",
        },
        "distributed_join": {
            "enabled": True,
            "broadcast_threshold": 5000,
        },
        "gsi": {
            "enabled": True,
            "state_file": ".quasar/gsi/indexes.json",
        },
        "matview": {
            "enabled": True,
            "state_file": ".quasar/matview/catalog.json",
            "default_interval_sec": 60,
            "max_failures": 5,
            "circuit_cooldown_sec": 30,
            "mutation_debounce_sec": 2,
            "on_mutation_enabled": True,
        },
    }
