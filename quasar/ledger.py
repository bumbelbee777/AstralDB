"""Ledger-style transfers and cross-shard reconciliation for financial workloads."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from quasar.saga import QuasarSaga, SagaStep
from quasar.security import sql_literal_safe as sql_literal

if TYPE_CHECKING:
    from quasar.quasar import QuasarCluster

_LEDGER_DDL = """
CREATE TABLE IF NOT EXISTS accounts (
  account_id TEXT PRIMARY KEY,
  balance_cents INTEGER NOT NULL DEFAULT 0,
  currency TEXT NOT NULL DEFAULT 'USD',
  updated_at TEXT
);
CREATE TABLE IF NOT EXISTS ledger_entries (
  entry_id TEXT PRIMARY KEY,
  txn_id TEXT NOT NULL,
  account_id TEXT NOT NULL,
  amount_cents INTEGER NOT NULL,
  side TEXT NOT NULL,
  counterparty TEXT,
  created_at TEXT NOT NULL
);
"""


@dataclass(frozen=True)
class FinancialConfig:
    enabled: bool = False
    currency: str = "USD"
    accounts_table: str = "accounts"
    ledger_table: str = "ledger_entries"
    bootstrap_schema: bool = True

    @classmethod
    def from_config(cls, raw: Optional[Dict[str, Any]]) -> "FinancialConfig":
        if not raw:
            return cls()
        return cls(
            enabled=bool(raw.get("enabled", False)),
            currency=str(raw.get("currency", "USD")),
            accounts_table=str(raw.get("accounts_table", "accounts")),
            ledger_table=str(raw.get("ledger_table", "ledger_entries")),
            bootstrap_schema=bool(raw.get("bootstrap_schema", True)),
        )


@dataclass
class TransferRequest:
    txn_id: str
    from_account: str
    to_account: str
    amount_cents: int
    from_shard_key: str
    to_shard_key: str
    currency: str = "USD"

    def validate(self) -> None:
        if self.amount_cents <= 0:
            raise ValueError("amount_cents must be positive")
        if not self.txn_id.strip():
            raise ValueError("txn_id is required")


@dataclass
class TransferResult:
    txn_id: str
    status: str
    saga: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {"txn_id": self.txn_id, "status": self.status, "saga": self.saga}


class QuasarLedger:
    """Double-entry transfer across shards using saga + MVCC snapshot balance checks."""

    def __init__(
        self,
        cluster: "QuasarCluster",
        config: FinancialConfig,
        *,
        saga: QuasarSaga,
    ) -> None:
        self.cluster = cluster
        self.config = config
        self.saga = saga

    def bootstrap(self) -> None:
        if not self.config.bootstrap_schema:
            return
        self.cluster.execute(_LEDGER_DDL.strip())

    def _debit_sql(self, req: TransferRequest) -> str:
        a = sql_literal(req.from_account)
        amt = int(req.amount_cents)
        txn = sql_literal(req.txn_id)
        cur = sql_literal(req.currency)
        return (
            f"UPDATE accounts SET balance_cents = balance_cents - {amt}, "
            f"updated_at = datetime('now') WHERE account_id = {a} AND currency = {cur} "
            f"AND balance_cents >= {amt};\n"
            f"INSERT INTO ledger_entries (entry_id, txn_id, account_id, amount_cents, side, "
            f"counterparty, created_at) VALUES ({sql_literal(req.txn_id + ':d')}, {txn}, {a}, "
            f"{-amt}, 'debit', {sql_literal(req.to_account)}, datetime('now'));"
        )

    def _credit_sql(self, req: TransferRequest) -> str:
        a = sql_literal(req.to_account)
        amt = int(req.amount_cents)
        txn = sql_literal(req.txn_id)
        cur = sql_literal(req.currency)
        return (
            f"UPDATE accounts SET balance_cents = balance_cents + {amt}, "
            f"updated_at = datetime('now') WHERE account_id = {a} AND currency = {cur};\n"
            f"INSERT INTO ledger_entries (entry_id, txn_id, account_id, amount_cents, side, "
            f"counterparty, created_at) VALUES ({sql_literal(req.txn_id + ':c')}, {txn}, {a}, "
            f"{amt}, 'credit', {sql_literal(req.from_account)}, datetime('now'));"
        )

    def _compensate_debit_sql(self, req: TransferRequest) -> str:
        a = sql_literal(req.from_account)
        amt = int(req.amount_cents)
        txn = sql_literal(req.txn_id + ":rev:d")
        return (
            f"UPDATE accounts SET balance_cents = balance_cents + {amt} WHERE account_id = {a};\n"
            f"INSERT INTO ledger_entries (entry_id, txn_id, account_id, amount_cents, side, "
            f"counterparty, created_at) VALUES ({sql_literal(req.txn_id + ':rev:d')}, {txn}, {a}, "
            f"{amt}, 'compensate', {sql_literal(req.to_account)}, datetime('now'));"
        )

    def _compensate_credit_sql(self, req: TransferRequest) -> str:
        a = sql_literal(req.to_account)
        amt = int(req.amount_cents)
        txn = sql_literal(req.txn_id + ":rev:c")
        return (
            f"UPDATE accounts SET balance_cents = balance_cents - {amt} WHERE account_id = {a};\n"
            f"INSERT INTO ledger_entries (entry_id, txn_id, account_id, amount_cents, side, "
            f"counterparty, created_at) VALUES ({sql_literal(req.txn_id + ':rev:c')}, {txn}, {a}, "
            f"{-amt}, 'compensate', {sql_literal(req.from_account)}, datetime('now'));"
        )

    def transfer(self, req: TransferRequest) -> TransferResult:
        req.validate()
        self.bootstrap()
        steps = [
            SagaStep(
                name="debit",
                sql=self._debit_sql(req),
                shard_key=req.from_shard_key,
                compensate_sql=self._compensate_debit_sql(req),
            ),
            SagaStep(
                name="credit",
                sql=self._credit_sql(req),
                shard_key=req.to_shard_key,
                compensate_sql=self._compensate_credit_sql(req),
            ),
        ]
        saga_result = self.saga.run(req.txn_id, steps)
        status = "posted" if saga_result.status == "completed" else saga_result.status
        return TransferResult(txn_id=req.txn_id, status=status, saga=saga_result.to_dict())


@dataclass
class ReconcileReport:
    consistent: bool
    per_shard: Dict[str, str]
    outliers: List[str]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "consistent": self.consistent,
            "per_shard": self.per_shard,
            "outliers": self.outliers,
        }


class QuasarReconciler:
    """Compare aggregate ledger totals or custom probe SQL across shards."""

    def __init__(self, cluster: "QuasarCluster") -> None:
        self.cluster = cluster

    def sum_balances(self) -> ReconcileReport:
        sql = "SELECT COALESCE(SUM(balance_cents), 0) FROM accounts;"
        merged = self.cluster.execute_merged(sql)
        per_shard = {row["node"]: row["stdout"].strip() for row in merged.per_shard}
        values = set(per_shard.values())
        baseline = next(iter(values)) if values else None
        outliers = [n for n, v in per_shard.items() if v != baseline]
        return ReconcileReport(
            consistent=len(values) <= 1,
            per_shard=per_shard,
            outliers=outliers,
        )

    def probe(self, sql: str) -> ReconcileReport:
        merged = self.cluster.execute_merged(sql)
        per_shard = {row["node"]: row["stdout"].strip() for row in merged.per_shard}
        values = set(per_shard.values())
        baseline = next(iter(values)) if values else None
        outliers = [n for n, v in per_shard.items() if v != baseline]
        return ReconcileReport(
            consistent=len(values) <= 1,
            per_shard=per_shard,
            outliers=outliers,
        )
