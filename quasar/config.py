"""Load and validate Quasar cluster JSON configs."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional, Union

from quasar.security import (
    SecurityPolicy,
    load_json_config_file,
    resolve_cluster_path,
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
        return str(resolve_cluster_path(base, value, allow_outside=allow_outside))

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
            "shards": {},
        },
        "rebalance": {"shard_key_column": "id", "auto_apply": False},
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
        "workload": {"circuit_failure_threshold": 5, "circuit_cooldown_sec": 30},
        "security": {
            "max_sql_bytes": 524288,
            "require_gateway_auth": False,
            "redact_secrets_in_errors": True,
            "allow_path_outside_config_root": False,
        },
    }
