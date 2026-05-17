"""Quasar — orchestration layer for AstralDB production deployments."""

from quasar.client import AstralDBClient, QueryResult, find_astraldb
from quasar.config import ConfigError, load_cluster_config
from quasar.errors import (
    QuasarCircuitOpenError,
    QuasarError,
    QuasarOverloadError,
    QuasarSecurityError,
)
from quasar.security import SecurityPolicy
from quasar.ring import ConsistentHashRing
from quasar.crossjoin import QuasarCrossShardJoin
from quasar.drift import QuasarDrift
from quasar.pool import PooledAstralDBClient, PoolConfig, create_client
from quasar.region import QuasarMultiRegion
from quasar.xtxn import CrossShardTransaction
from quasar.inventory import QuasarInventory
from quasar.ops import QuasarRolling, QuasarWatch, repair_replica_set, shard_ring_map
from quasar.quasar import (
    QUASAR_VERSION,
    QuasarBackup,
    QuasarCluster,
    QuasarMigration,
    QuasarMonitor,
    QuasarReplica,
    QuasarShard,
    ShardNode,
    scaffold_cluster,
)
from quasar.sqlbatch import QuasarSqlBatch, parse_batch_file

__all__ = [
    "QUASAR_VERSION",
    "AstralDBClient",
    "QueryResult",
    "find_astraldb",
    "ConfigError",
    "load_cluster_config",
    "ConsistentHashRing",
    "QuasarBackup",
    "QuasarCluster",
    "QuasarMigration",
    "QuasarMonitor",
    "QuasarReplica",
    "QuasarShard",
    "ShardNode",
    "scaffold_cluster",
    "QuasarInventory",
    "QuasarDrift",
    "QuasarRolling",
    "QuasarWatch",
    "QuasarSqlBatch",
    "parse_batch_file",
    "repair_replica_set",
    "shard_ring_map",
    "PooledAstralDBClient",
    "PoolConfig",
    "create_client",
    "CrossShardTransaction",
    "QuasarMultiRegion",
    "QuasarCrossShardJoin",
    "QuasarError",
    "QuasarSecurityError",
    "QuasarOverloadError",
    "QuasarCircuitOpenError",
    "SecurityPolicy",
]
