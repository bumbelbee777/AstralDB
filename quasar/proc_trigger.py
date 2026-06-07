"""Stored procedure and trigger migration helpers for AstralDB."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

from quasar.client import AstralDBClient
from quasar.sql_util import split_sql_script

PathLike = Union[str, Path]

DIALECT_ASTRAL = "astral"
DIALECT_PLSQL = "plsql"
DIALECT_PLSQL_ALT = "plpgsql"
DIALECT_TSQL = "tsql"


@dataclass
class ProcedureSpec:
    name: str
    sql: str
    dialect: str = DIALECT_ASTRAL
    source: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return {"name": self.name, "sql": self.sql, "dialect": self.dialect, "source": self.source}


@dataclass
class TriggerSpec:
    name: str
    sql: str
    table: str = ""
    timing: str = ""
    event: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "sql": self.sql,
            "table": self.table,
            "timing": self.timing,
            "event": self.event,
        }


@dataclass
class ProcTriggerApplyResult:
    procedures_applied: int = 0
    procedures_failed: List[str] = field(default_factory=list)
    triggers_applied: int = 0
    triggers_failed: List[str] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "procedures_applied": self.procedures_applied,
            "procedures_failed": list(self.procedures_failed),
            "triggers_applied": self.triggers_applied,
            "triggers_failed": list(self.triggers_failed),
            "notes": list(self.notes),
        }


def normalize_procedure_sql(sql: str, *, name: Optional[str] = None) -> str:
    text = sql.strip()
    if not text:
        raise ValueError("empty procedure SQL")
    upper = text.upper()
    if upper.startswith("CREATE PROCEDURE") or upper.startswith("CREATE OR REPLACE PROCEDURE"):
        return text if text.endswith(";") else text + ";"
    proc_name = name or "migrated_proc"
    body = text
    if body.startswith("(") and body.endswith(")"):
        return f"CREATE PROCEDURE {proc_name} AS {body};"
    if "BEGIN" in upper and "END" in upper:
        if "@" in text or "SET NOCOUNT" in upper or "BEGIN TRY" in upper or "CREATE OR ALTER" in upper:
            return text if text.endswith(";") else text + ";"
        return f"CREATE PROCEDURE {proc_name} LANGUAGE plpgsql AS $$\n{body}\n$$;"
    return f"CREATE PROCEDURE {proc_name} AS (\n{body}\n);"


def normalize_trigger_sql(sql: str) -> str:
    text = sql.strip()
    if not text:
        raise ValueError("empty trigger SQL")
    upper = text.upper()
    if upper.startswith("CREATE TRIGGER") or upper.startswith("CREATE OR REPLACE TRIGGER"):
        return text if text.endswith(";") else text + ";"
    raise ValueError("trigger SQL must be a CREATE TRIGGER statement")


def specs_from_bundle(bundle: Dict[str, Any]) -> tuple[List[ProcedureSpec], List[TriggerSpec]]:
    procs: List[ProcedureSpec] = []
    triggers: List[TriggerSpec] = []
    for raw in bundle.get("procedures") or []:
        if isinstance(raw, dict) and raw.get("sql"):
            procs.append(
                ProcedureSpec(
                    name=str(raw.get("name") or "proc"),
                    sql=str(raw["sql"]),
                    dialect=str(raw.get("dialect") or DIALECT_ASTRAL),
                    source=str(raw.get("source") or ""),
                )
            )
    for raw in bundle.get("triggers") or []:
        if isinstance(raw, dict) and raw.get("sql"):
            triggers.append(
                TriggerSpec(
                    name=str(raw.get("name") or "trig"),
                    sql=str(raw["sql"]),
                    table=str(raw.get("table") or ""),
                    timing=str(raw.get("timing") or ""),
                    event=str(raw.get("event") or ""),
                )
            )
    return procs, triggers


def attach_proc_trigger_to_bundle(
    bundle: Dict[str, Any],
    *,
    procedures: Optional[List[ProcedureSpec]] = None,
    triggers: Optional[List[TriggerSpec]] = None,
) -> Dict[str, Any]:
    out = dict(bundle)
    if procedures:
        out["procedures"] = [p.to_dict() for p in procedures]
    if triggers:
        out["triggers"] = [t.to_dict() for t in triggers]
    return out


class QuasarProcTrigger:
    """Apply stored procedures and triggers on an AstralDB database file."""

    def __init__(self, client: Optional[AstralDBClient] = None) -> None:
        self.client = client or AstralDBClient()

    def apply(
        self,
        database: PathLike,
        *,
        procedures: Optional[List[ProcedureSpec]] = None,
        triggers: Optional[List[TriggerSpec]] = None,
        script_path: Optional[PathLike] = None,
        best_effort: bool = True,
    ) -> ProcTriggerApplyResult:
        result = ProcTriggerApplyResult()
        db = Path(database)

        if script_path is not None:
            self.client.script(script_path, database=db)
            result.notes.append(f"executed script {Path(script_path).name}")
            return result

        for spec in procedures or []:
            try:
                sql = normalize_procedure_sql(spec.sql, name=spec.name)
                self.client.query(sql, database=db, immediate=True)
                result.procedures_applied += 1
            except Exception as err:
                result.procedures_failed.append(f"{spec.name}: {err}")
                if not best_effort:
                    raise

        for spec in triggers or []:
            try:
                sql = normalize_trigger_sql(spec.sql)
                self.client.query(sql, database=db, immediate=True)
                result.triggers_applied += 1
            except Exception as err:
                result.triggers_failed.append(f"{spec.name}: {err}")
                if not best_effort:
                    raise

        return result

    def apply_bundle_section(
        self,
        database: PathLike,
        bundle: Dict[str, Any],
        *,
        include_procedures: bool = True,
        include_triggers: bool = True,
        best_effort: bool = True,
    ) -> ProcTriggerApplyResult:
        procs, triggers = specs_from_bundle(bundle)
        if not include_procedures:
            procs = []
        if not include_triggers:
            triggers = []
        return self.apply(database, procedures=procs, triggers=triggers, best_effort=best_effort)

    def write_sidecar_scripts(
        self,
        work_dir: PathLike,
        *,
        procedures: Optional[List[ProcedureSpec]] = None,
        triggers: Optional[List[TriggerSpec]] = None,
    ) -> Dict[str, str]:
        work = Path(work_dir)
        work.mkdir(parents=True, exist_ok=True)
        paths: Dict[str, str] = {}
        if procedures:
            proc_path = work / "migrated_procedures.sql"
            chunks = [normalize_procedure_sql(p.sql, name=p.name) for p in procedures]
            proc_path.write_text("\n\n".join(chunks) + "\n", encoding="utf-8")
            paths["procedures"] = str(proc_path.resolve())
        if triggers:
            trig_path = work / "migrated_triggers.sql"
            chunks = [normalize_trigger_sql(t.sql) for t in triggers]
            trig_path.write_text("\n\n".join(chunks) + "\n", encoding="utf-8")
            paths["triggers"] = str(trig_path.resolve())
        combined: List[str] = []
        for key in ("procedures", "triggers"):
            if key in paths:
                combined.append(Path(paths[key]).read_text(encoding="utf-8"))
        if combined:
            all_path = work / "migrated_proc_trigger.sql"
            all_path.write_text("\n".join(combined), encoding="utf-8")
            paths["combined"] = str(all_path.resolve())
        return paths


_RE_CREATE_PROC = re.compile(
    r"^\s*CREATE\s+(?:OR\s+REPLACE\s+)?PROCEDURE\s+([A-Za-z_][A-Za-z0-9_]*)",
    re.IGNORECASE | re.MULTILINE,
)
_RE_CREATE_TRIG = re.compile(
    r"^\s*CREATE\s+(?:OR\s+REPLACE\s+)?TRIGGER\s+([A-Za-z_][A-Za-z0-9_]*)",
    re.IGNORECASE | re.MULTILINE,
)


def extract_proc_trigger_from_sql(text: str) -> tuple[List[ProcedureSpec], List[TriggerSpec]]:
    """Parse a SQL dump for CREATE PROCEDURE / CREATE TRIGGER statements."""
    procs: List[ProcedureSpec] = []
    triggers: List[TriggerSpec] = []
    for stmt in split_sql_script(text):
        upper = stmt.upper()
        if "CREATE PROCEDURE" in upper or "CREATE OR REPLACE PROCEDURE" in upper:
            m = _RE_CREATE_PROC.search(stmt)
            name = m.group(1) if m else f"proc_{len(procs)}"
            dialect = DIALECT_PLSQL_ALT if "LANGUAGE PLPGSQL" in upper else (
                DIALECT_PLSQL if " IS " in upper or " AS " in upper and "BEGIN" in upper else DIALECT_ASTRAL
            )
            procs.append(ProcedureSpec(name=name, sql=stmt, dialect=dialect))
        elif "CREATE TRIGGER" in upper or "CREATE OR REPLACE TRIGGER" in upper:
            m = _RE_CREATE_TRIG.search(stmt)
            name = m.group(1) if m else f"trig_{len(triggers)}"
            triggers.append(TriggerSpec(name=name, sql=stmt))
    return procs, triggers
