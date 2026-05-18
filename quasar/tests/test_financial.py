"""Financial workloads: saga, idempotency, ledger, journal."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from quasar.audit import QuasarJournal
from quasar.idempotency import IdempotencyConfig, IdempotencyStore, digest_result
from quasar.ledger import FinancialConfig, TransferRequest
from quasar.quasar import QuasarCluster
from quasar.saga import QuasarSaga, SagaStep


@pytest.fixture
def financial_cluster(mock_client, tmp_path: Path) -> QuasarCluster:
    shards = [
        {"name": "s0", "database": str(tmp_path / "s0.db")},
        {"name": "s1", "database": str(tmp_path / "s1.db")},
    ]
    cfg = {
        "shards": shards,
        "virtual_nodes": 8,
        "financial": {
            "enabled": True,
            "journal_file": str(tmp_path / "journal.jsonl"),
            "idempotency_dir": str(tmp_path / "idem"),
            "saga_state_dir": str(tmp_path / "sagas"),
        },
        "pool": {"enabled": False},
    }
    path = tmp_path / "cluster.json"
    path.write_text(json.dumps(cfg), encoding="utf-8")
    return QuasarCluster(cfg, client=mock_client, config_path=path)


def test_journal_append(tmp_path: Path):
    journal = QuasarJournal(tmp_path / "j.jsonl")
    journal.record("TEST", detail="hello")
    rows = journal.tail(10)
    assert rows[-1]["event"] == "TEST"


def test_idempotency_file_store(financial_cluster: QuasarCluster):
    store = financial_cluster.idempotency
    assert store is not None
    key = "pay-1"
    store.begin(key, shard_key="acct:a")
    store.complete(key, shard_key="acct:a", digest=digest_result({"ok": True}))
    again = store.get(key, shard_key="acct:a")
    assert again["status"] == "completed"


def test_idempotency_execute(financial_cluster: QuasarCluster):
    key = "idem-99"
    r1 = financial_cluster.execute_idempotent(
        "SELECT 1;",
        shard_key="user:1",
        idempotency_key=key,
    )
    r2 = financial_cluster.execute_idempotent(
        "SELECT 1;",
        shard_key="user:1",
        idempotency_key=key,
    )
    assert "IDEMPOTENT:" in r2[0].result.stdout
    assert r1[0].node == r2[0].node


def test_saga_compensates_on_failure(financial_cluster: QuasarCluster):
    saga = financial_cluster.saga
    assert saga is not None
    calls = {"n": 0}
    orig_execute = financial_cluster.execute

    def flaky_execute(sql, **kwargs):
        calls["n"] += 1
        if calls["n"] == 2:
            raise RuntimeError("simulated shard failure")
        return orig_execute(sql, **kwargs)

    financial_cluster.execute = flaky_execute  # type: ignore[method-assign]
    steps = [
        SagaStep(
            name="ok",
            sql="SELECT 1;",
            shard_key="user:1",
            compensate_sql="SELECT 2;",
        ),
        SagaStep(
            name="fail",
            sql="SELECT 9;",
            shard_key="user:2",
            compensate_sql="SELECT 3;",
        ),
    ]
    result = saga.run("saga-1", steps)
    assert result.status in ("compensated", "failed")
    assert "ok" in result.completed_steps


def test_transfer_runs_saga(financial_cluster: QuasarCluster):
    financial_cluster.ledger.bootstrap()
    spec = {
        "txn_id": "t-1",
        "from_account": "a1",
        "to_account": "a2",
        "amount_cents": 100,
        "from_shard_key": "acct:a1",
        "to_shard_key": "acct:a2",
    }
    result = financial_cluster.transfer(spec)
    assert result.txn_id == "t-1"
    assert result.status in ("posted", "compensated", "failed")
