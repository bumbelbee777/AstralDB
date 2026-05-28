"""Quasar — orchestration layer for AstralDB production deployments."""

from quasar.client import AstralDBClient, QueryResult, find_astraldb
from quasar.config import ConfigError, load_cluster_config
from quasar.errors import (
    QuasarCircuitOpenError,
    QuasarError,
    QuasarLockError,
    QuasarOverloadError,
    QuasarRestoreError,
    QuasarSecurityError,
)
from quasar.merge import MergedFanoutResult, merge_routed_results, looks_like_read_query
from quasar.locks import DatabaseLock, lock_databases
from quasar.security import SecurityPolicy
from quasar.ring import ConsistentHashRing
from quasar.crossjoin import QuasarCrossShardJoin
from quasar.drift import QuasarDrift
from quasar.pool import PooledAstralDBClient, PoolConfig, create_client
from quasar.recovery import RetryPolicy, RetryStats, execute_with_retry, is_transient_error
from quasar.mvcc import MvccConfig, prepare_read_sql, wrap_snapshot_transaction
from quasar.audit import QuasarJournal
from quasar.idempotency import IdempotencyConfig, IdempotencyStore
from quasar.saga import QuasarSaga, SagaStep, SagaResult
from quasar.ledger import (
    FinancialConfig,
    QuasarLedger,
    QuasarReconciler,
    TransferRequest,
    TransferResult,
)
from quasar.region import QuasarMultiRegion
from quasar.xtxn import CrossShardTransaction
from quasar.dtxn import DistributedTransactionCoordinator, DistributedTxnConfig
from quasar.failover import FailoverPolicy
from quasar.recovery_auto import CrashRecoveryManager, RecoveryAutomationConfig
from quasar.crossquery import CrossQueryResult, CrossQuerySpec, QuasarCrossQuery, parse_stdout_rows
from quasar.autoscale import AutoscaleConfig, AutoscaleDecision, QuasarAutoscale
from quasar.consensus import ClusterConsensus, ConsensusConfig, build_consensus
from quasar.fdw import FdwManager, FdwResult, ForeignSource, ForeignTable
from quasar.gsi import GlobalSecondaryIndex, GlobalSecondaryIndexManager
from quasar.matview import MaterializedViewManager, MaterializedViewSpec
from quasar.migrate_util import MigrationPlan, QuasarMigrate, detect_migration_mode, is_sqlite_file
from quasar.migrate_sources import export_external_bundle, is_database_uri, parse_database_uri
from quasar.proc_trigger import ProcedureSpec, QuasarProcTrigger, TriggerSpec
from quasar.gateway_keys import GatewayKeyStore, format_api_token
from quasar.cdc import BinaryFileCdcSink, CdcCheckpointStore, CdcEvent, FileCdcSink, QuasarCdcPublisher
from quasar.binary_ipc import decode_frames, encode_frames
from quasar.distjoin import DistributedJoinPlan, DistributedJoinPlanner, QuasarDistributedJoinExecutor
from quasar.edge import EdgeNode, EdgeRegistry
from quasar.serverless import QuasarServerlessController
from quasar.splitmerge import QuasarSplitMergeController
from quasar.ops import QuasarUpgradeController
from quasar.rebalance_auto import QuasarRebalanceAutomator, RebalanceAutomationConfig
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
    "QuasarRestoreError",
    "QuasarLockError",
    "SecurityPolicy",
    "MergedFanoutResult",
    "merge_routed_results",
    "looks_like_read_query",
    "DatabaseLock",
    "lock_databases",
    "RetryPolicy",
    "RetryStats",
    "execute_with_retry",
    "is_transient_error",
    "MvccConfig",
    "prepare_read_sql",
    "wrap_snapshot_transaction",
    "QuasarJournal",
    "IdempotencyConfig",
    "IdempotencyStore",
    "QuasarSaga",
    "SagaStep",
    "SagaResult",
    "FinancialConfig",
    "QuasarLedger",
    "QuasarReconciler",
    "TransferRequest",
    "TransferResult",
    "DistributedTransactionCoordinator",
    "DistributedTxnConfig",
    "FailoverPolicy",
    "CrashRecoveryManager",
    "RecoveryAutomationConfig",
    "QuasarCrossQuery",
    "CrossQuerySpec",
    "CrossQueryResult",
    "parse_stdout_rows",
    "QuasarAutoscale",
    "AutoscaleConfig",
    "AutoscaleDecision",
    "ClusterConsensus",
    "ConsensusConfig",
    "build_consensus",
    "QuasarRebalanceAutomator",
    "RebalanceAutomationConfig",
    "FdwManager",
    "FdwResult",
    "ForeignSource",
    "ForeignTable",
    "GlobalSecondaryIndex",
    "GlobalSecondaryIndexManager",
    "MaterializedViewManager",
    "MaterializedViewSpec",
    "QuasarMigrate",
    "MigrationPlan",
    "detect_migration_mode",
    "is_sqlite_file",
    "is_database_uri",
    "parse_database_uri",
    "export_external_bundle",
    "ProcedureSpec",
    "TriggerSpec",
    "QuasarProcTrigger",
    "GatewayKeyStore",
    "format_api_token",
    "QuasarUpgradeController",
    "CdcCheckpointStore",
    "CdcEvent",
    "FileCdcSink",
    "QuasarCdcPublisher",
    "DistributedJoinPlan",
    "DistributedJoinPlanner",
    "QuasarDistributedJoinExecutor",
    "EdgeNode",
    "EdgeRegistry",
    "QuasarServerlessController",
    "QuasarSplitMergeController",
    "BinaryFileCdcSink",
    "encode_frames",
    "decode_frames",
]
