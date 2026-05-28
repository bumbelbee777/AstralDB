"""Migration utilities: SQLite, SQL scripts, bundles, external DBs, and AstralDB moves."""

from __future__ import annotations

import json
import re
import sqlite3
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

from quasar.bundle import BUNDLE_KIND, BUNDLE_VERSION
from quasar.client import AstralDBClient
from quasar.migrate_sources import (
    describe_external_source,
    export_external_bundle,
    is_database_uri,
    parse_database_uri,
)
from quasar.proc_trigger import (
    QuasarProcTrigger,
    TriggerSpec,
    attach_proc_trigger_to_bundle,
    extract_proc_trigger_from_sql,
)
from quasar.quasar import QuasarMigration

PathLike = Union[str, Path]

SQLITE_MAGIC = b"SQLite format 3\x00"


@dataclass
class MigrationPlan:
    mode: str
    source: str
    target: str
    tables: List[str] = field(default_factory=list)
    row_counts: Dict[str, int] = field(default_factory=dict)
    notes: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "mode": self.mode,
            "source": self.source,
            "target": self.target,
            "tables": list(self.tables),
            "row_counts": dict(self.row_counts),
            "notes": list(self.notes),
        }


def is_sqlite_file(path: PathLike) -> bool:
    path = Path(path)
    if not path.is_file():
        return False
    try:
        with path.open("rb") as fh:
            return fh.read(len(SQLITE_MAGIC)) == SQLITE_MAGIC
    except OSError:
        return False


def detect_migration_mode(source: Union[PathLike, str]) -> str:
    if isinstance(source, str) and is_database_uri is not None and is_database_uri(source):
        return "external"
    path = Path(source)
    suffix = path.suffix.lower()
    if suffix == ".sql":
        return "script"
    if suffix in (".json", ".bundle"):
        return "bundle"
    if suffix in (".sqlite", ".sqlite3") or (suffix == ".db" and is_sqlite_file(path)):
        return "sqlite"
    if suffix == ".db":
        return "astral"
    if suffix in (".csv", ".tsv"):
        return "bundle"
    raise ValueError(
        f"cannot detect migration mode for {path} "
        "(use --mode astral|sqlite|bundle|script|copy|external or a database URI)"
    )


def _sqlite_type_to_astral(decl: str) -> str:
    d = (decl or "TEXT").upper()
    if "INT" in d:
        return "INT"
    if "REAL" in d or "FLOA" in d or "DOUB" in d:
        return "REAL"
    if "BLOB" in d:
        return "BLOB"
    return "TEXT"


def sqlite_tables(conn: sqlite3.Connection, *, only: Optional[List[str]] = None) -> List[str]:
    cur = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
    )
    names = [str(r[0]) for r in cur.fetchall()]
    if only:
        wanted = {t.strip() for t in only if t.strip()}
        names = [n for n in names if n in wanted]
    return names


def sqlite_to_bundle(
    sqlite_path: PathLike,
    *,
    tables: Optional[List[str]] = None,
) -> Dict[str, Any]:
    path = Path(sqlite_path)
    conn = sqlite3.connect(str(path))
    conn.row_factory = sqlite3.Row
    try:
        table_names = sqlite_tables(conn, only=tables)
        out_tables: Dict[str, Any] = {}
        for table in table_names:
            cols = conn.execute(f"PRAGMA table_info({table})").fetchall()
            schema: List[Dict[str, Any]] = []
            col_names: List[str] = []
            for col in cols:
                name = str(col[1])
                col_names.append(name)
                schema.append(
                    {
                        "name": name,
                        "sqlType": _sqlite_type_to_astral(str(col[2])),
                        "pk": bool(col[5]),
                        "unique": False,
                        "notNull": bool(col[3]),
                    }
                )
            rows: List[Dict[str, str]] = []
            for row in conn.execute(f"SELECT * FROM {table}"):
                item: Dict[str, str] = {}
                for name in col_names:
                    val = row[name]
                    item[name] = "" if val is None else str(val)
                rows.append(item)
            out_tables[table] = {"schema": schema, "rows": rows}
        return {
            "bundleVersion": BUNDLE_VERSION,
            "kind": BUNDLE_KIND,
            "tables": out_tables,
        }
    finally:
        conn.close()


from quasar.sql_util import split_sql_script

class QuasarMigrate:
    """High-level migrations into AstralDB database files."""

    def __init__(self, client: Optional[AstralDBClient] = None) -> None:
        self.client = client or AstralDBClient()
        self.proc_trigger = QuasarProcTrigger(self.client)

    def plan(
        self,
        source: Union[PathLike, str],
        target: PathLike,
        *,
        mode: str = "auto",
        tables: Optional[List[str]] = None,
        include_procedures: bool = True,
        include_triggers: bool = True,
        source_schema: Optional[str] = None,
    ) -> MigrationPlan:
        src_str = str(source)
        tgt = Path(target)
        resolved = mode if mode != "auto" else detect_migration_mode(source)
        src_display = src_str if resolved == "external" else str(Path(source).resolve())
        plan = MigrationPlan(mode=resolved, source=src_display, target=str(tgt.resolve()))
        if resolved == "external":
            info = describe_external_source(src_str)
            plan.notes.extend(info.notes)
            if include_procedures:
                plan.notes.append("Stored procedures will be exported when the driver supports catalog queries.")
            if include_triggers:
                plan.notes.append("Triggers will be exported and applied after table import (best-effort).")
        if resolved == "sqlite":
            src = Path(source)
            conn = sqlite3.connect(str(src))
            try:
                names = sqlite_tables(conn, only=tables)
                plan.tables = names
                for name in names:
                    n = conn.execute(f"SELECT COUNT(*) FROM {name}").fetchone()
                    plan.row_counts[name] = int(n[0]) if n else 0
            finally:
                conn.close()
            plan.notes.append("SQLite data will be converted to an AstralDB JSON bundle, then imported.")
        elif resolved == "astral":
            plan.notes.append("Uses export/import bundle or --copy for same-host file migration.")
        elif resolved == "script":
            src = Path(source)
            text = src.read_text(encoding="utf-8", errors="replace")
            stmts = split_sql_script(text)
            procs, trigs = extract_proc_trigger_from_sql(text)
            plan.notes.append(f"Will execute {len(stmts)} SQL statement(s) on the target database.")
            if include_procedures and procs:
                plan.notes.append(f"Found {len(procs)} CREATE PROCEDURE statement(s) in script.")
            if include_triggers and trigs:
                plan.notes.append(f"Found {len(trigs)} CREATE TRIGGER statement(s) in script.")
        elif resolved == "bundle":
            raw = json.loads(Path(source).read_text(encoding="utf-8"))
            if isinstance(raw.get("tables"), dict):
                plan.tables = sorted(raw["tables"].keys())
                for name in plan.tables:
                    rows = raw["tables"][name].get("rows", [])
                    plan.row_counts[name] = len(rows) if isinstance(rows, list) else 0
        return plan

    def run(
        self,
        source: Union[PathLike, str],
        target: PathLike,
        *,
        mode: str = "auto",
        tables: Optional[List[str]] = None,
        work_dir: Optional[PathLike] = None,
        bundle_format: str = "json",
        use_copy: bool = False,
        dry_run: bool = False,
        include_procedures: bool = True,
        include_triggers: bool = True,
        source_schema: Optional[str] = None,
        apply_proc_trigger: bool = True,
    ) -> Dict[str, Any]:
        if dry_run:
            return self.plan(
                source,
                target,
                mode=mode,
                tables=tables,
                include_procedures=include_procedures,
                include_triggers=include_triggers,
                source_schema=source_schema,
            ).to_dict()

        src_str = str(source)
        tgt = Path(target)
        resolved = mode if mode != "auto" else detect_migration_mode(source)
        tgt.parent.mkdir(parents=True, exist_ok=True)

        if resolved == "copy" or (resolved == "astral" and use_copy):
            QuasarMigration(src, tgt, client=self.client).migrate_via_checkpoint_copy()
            return {"mode": "copy", "target": str(tgt.resolve())}

        if resolved == "astral":
            bundle = QuasarMigration(src, tgt, client=self.client, bundle_format=bundle_format).migrate_via_bundle(
                work_dir=work_dir
            )
            return {"mode": "astral", "target": str(tgt.resolve()), "bundle": str(bundle)}

        if resolved == "external":
            bundle_data = export_external_bundle(
                src_str,
                tables=tables,
                schema=source_schema,
                include_procedures=include_procedures,
                include_triggers=include_triggers,
            )
            work = Path(work_dir) if work_dir else Path.cwd() / ".quasar_migrate"
            work.mkdir(parents=True, exist_ok=True)
            profile = parse_database_uri(src_str) if parse_database_uri else None
            label = profile.engine if profile else "external"
            bundle_path = work / f"{label}_{int(time.time())}.bundle.json"
            bundle_path.write_text(json.dumps(bundle_data, indent=2), encoding="utf-8")
            result = self._import_bundle(
                bundle_path,
                tgt,
                fmt="json",
                extra={"mode": "external", "tables": list(bundle_data.get("tables", {}).keys())},
            )
            if apply_proc_trigger:
                pt = self.proc_trigger.apply_bundle_section(
                    tgt,
                    bundle_data,
                    include_procedures=include_procedures,
                    include_triggers=include_triggers,
                )
                result["proc_trigger"] = pt.to_dict()
            return result

        src = Path(source)

        if resolved == "sqlite":
            bundle_data = sqlite_to_bundle(src, tables=tables)
            if include_triggers:
                conn = sqlite3.connect(str(src))
                try:
                    trigs = [
                        TriggerSpec(name=str(row[0]), sql=str(row[1]))
                        for row in conn.execute(
                            "SELECT name, sql FROM sqlite_master WHERE type='trigger' AND sql IS NOT NULL"
                        )
                    ]
                    if trigs:
                        bundle_data = attach_proc_trigger_to_bundle(bundle_data, triggers=trigs)
                finally:
                    conn.close()
            work = Path(work_dir) if work_dir else Path.cwd() / ".quasar_migrate"
            work.mkdir(parents=True, exist_ok=True)
            bundle_path = work / f"sqlite_{int(time.time())}.bundle.json"
            bundle_path.write_text(json.dumps(bundle_data, indent=2), encoding="utf-8")
            result = self._import_bundle(
                bundle_path, tgt, fmt="json", extra={"mode": "sqlite", "tables": list(bundle_data["tables"].keys())}
            )
            if apply_proc_trigger and (bundle_data.get("procedures") or bundle_data.get("triggers")):
                result["proc_trigger"] = self.proc_trigger.apply_bundle_section(
                    tgt, bundle_data, include_procedures=include_procedures, include_triggers=include_triggers
                ).to_dict()
            return result

        if resolved == "bundle":
            raw = json.loads(src.read_text(encoding="utf-8"))
            result = self._import_bundle(src, tgt, fmt=bundle_format, extra={"mode": "bundle"})
            if apply_proc_trigger:
                result["proc_trigger"] = self.proc_trigger.apply_bundle_section(
                    tgt, raw, include_procedures=include_procedures, include_triggers=include_triggers
                ).to_dict()
            return result

        if resolved == "script":
            return self._run_script(src, tgt, include_procedures=include_procedures, include_triggers=include_triggers, apply_proc_trigger=apply_proc_trigger)

        raise ValueError(f"unsupported migration mode: {resolved}")

    def _import_bundle(self, bundle_path: Path, target: Path, *, fmt: str, extra: Dict[str, Any]) -> Dict[str, Any]:
        if target.exists():
            target.unlink()
        wal = Path(str(target) + ".wal")
        if wal.exists():
            wal.unlink()
        self.client.import_bundle(bundle_path, database=target, fmt=fmt)
        return {"target": str(target.resolve()), "bundle": str(bundle_path.resolve()), **extra}

    def _run_script(
        self,
        script_path: Path,
        target: Path,
        *,
        include_procedures: bool = True,
        include_triggers: bool = True,
        apply_proc_trigger: bool = True,
    ) -> Dict[str, Any]:
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            self.client.checkpoint_sql(database=target)
        text = script_path.read_text(encoding="utf-8", errors="replace")
        procs, trigs = extract_proc_trigger_from_sql(text)
        statements = split_sql_script(text)
        executed = 0
        for stmt in statements:
            if re.match(r"^\s*$", stmt):
                continue
            upper = stmt.upper()
            if "CREATE PROCEDURE" in upper or "CREATE TRIGGER" in upper:
                continue
            self.client.query(stmt if stmt.rstrip().endswith(";") else stmt + ";", database=target, immediate=True)
            executed += 1
        result: Dict[str, Any] = {
            "mode": "script",
            "target": str(target.resolve()),
            "statements_executed": executed,
        }
        if apply_proc_trigger and (procs or trigs):
            pt = self.proc_trigger.apply(
                target,
                procedures=procs if include_procedures else [],
                triggers=trigs if include_triggers else [],
            )
            result["proc_trigger"] = pt.to_dict()
        return result
