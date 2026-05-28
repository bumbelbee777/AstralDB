from __future__ import annotations

from pathlib import Path

from quasar.proc_trigger import (
    extract_proc_trigger_from_sql,
    normalize_procedure_sql,
    normalize_trigger_sql,
    specs_from_bundle,
)


def test_extract_proc_trigger_from_sql():
    text = """
    CREATE TABLE t (id INT);
    CREATE PROCEDURE bump AS (UPDATE t SET id = id + 1;);
    CREATE TRIGGER t_ai AFTER INSERT ON t FOR EACH ROW AS (INSERT INTO t VALUES (1););
    """
    procs, trigs = extract_proc_trigger_from_sql(text)
    assert len(procs) == 1
    assert procs[0].name == "bump"
    assert len(trigs) == 1
    assert trigs[0].name == "t_ai"


def test_normalize_procedure_and_trigger():
    sql = normalize_procedure_sql("SELECT 1;", name="seed")
    assert sql.upper().startswith("CREATE PROCEDURE SEED")
    trig = normalize_trigger_sql(
        "CREATE TRIGGER x AFTER INSERT ON t FOR EACH ROW AS (SELECT 1;);"
    )
    assert "CREATE TRIGGER" in trig.upper()


def test_specs_from_bundle():
    bundle = {
        "procedures": [{"name": "p1", "sql": "CREATE PROCEDURE p1 AS (SELECT 1;);"}],
        "triggers": [{"name": "tr1", "sql": "CREATE TRIGGER tr1 AFTER INSERT ON t FOR EACH ROW AS (SELECT 1;);"}],
    }
    procs, trigs = specs_from_bundle(bundle)
    assert len(procs) == 1
    assert len(trigs) == 1


def test_apply_procedure_on_astraldb(astraldb_client, tmp_path: Path):
    from quasar.proc_trigger import ProcedureSpec, QuasarProcTrigger

    db = tmp_path / "proc.db"
    astraldb_client.query("CREATE TABLE counters (id INT, n INT);", database=db, immediate=True)
    astraldb_client.query("INSERT INTO counters VALUES (1, 0);", database=db, immediate=True)
    pt = QuasarProcTrigger(astraldb_client)
    result = pt.apply(
        db,
        procedures=[
            ProcedureSpec(
                name="inc",
                sql="CREATE PROCEDURE inc AS (UPDATE counters SET n = n + 1 WHERE id = 1;);",
            )
        ],
    )
    assert result.procedures_applied == 1
    astraldb_client.query("CALL inc;", database=db, immediate=True)
    row = astraldb_client.query("SELECT n FROM counters WHERE id = 1;", database=db, immediate=True)
    assert row.ok
