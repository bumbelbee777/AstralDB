"""Export schema, data, procedures, and triggers from external databases into AstralDB bundles."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import urlparse, unquote

from quasar.bundle import BUNDLE_KIND, BUNDLE_VERSION
from quasar.proc_trigger import ProcedureSpec, TriggerSpec

# pip install quasar[migrate]  -> sqlalchemy + common drivers
_MIGRATE_EXTRA_HINT = "pip install 'quasar[migrate]' (sqlalchemy) and the driver for your database"


def _require_sqlalchemy():
    try:
        import sqlalchemy  # noqa: F401
        from sqlalchemy import create_engine, inspect, text
        from sqlalchemy.engine import Engine

        return create_engine, inspect, text, Engine
    except ImportError as exc:
        raise ImportError(
            f"SQLAlchemy is required for external database migration. {_MIGRATE_EXTRA_HINT}"
        ) from exc


@dataclass
class ExternalDbProfile:
    engine: str
    dsn: str
    display_name: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return {"engine": self.engine, "dsn": self.dsn, "display_name": self.display_name or self.engine}


def is_database_uri(source: str) -> bool:
    s = source.strip()
    if "://" not in s:
        return False
    scheme = s.split("://", 1)[0].lower()
    return "+" in scheme or scheme in (
        "postgresql",
        "postgres",
        "mysql",
        "mariadb",
        "oracle",
        "duckdb",
        "redshift",
        "awsathena",
        "athena",
        "sqlite",
        "mssql",
        "snowflake",
    )


def normalize_engine_name(scheme: str) -> str:
    base = scheme.split("+", 1)[0].lower()
    aliases = {
        "postgresql": "postgres",
        "postgres": "postgres",
        "mariadb": "mysql",
        "awsathena": "athena",
        "athena": "athena",
        "redshift": "postgres",
        "mssql": "mssql",
        "sqlserver": "mssql",
    }
    return aliases.get(base, base)


def parse_database_uri(uri: str) -> ExternalDbProfile:
    parsed = urlparse(uri.strip())
    if not parsed.scheme:
        raise ValueError(f"invalid database URI: {uri}")
    engine = normalize_engine_name(parsed.scheme)
    return ExternalDbProfile(engine=engine, dsn=uri.strip(), display_name=engine)


def detect_source_kind(source: str) -> str:
    if is_database_uri(source):
        return "external"
    return "path"


def _sql_type_to_astral(type_name: str) -> str:
    t = (type_name or "TEXT").upper()
    if any(x in t for x in ("INT", "SERIAL", "BIGINT", "SMALLINT", "TINYINT")):
        return "INT"
    if any(x in t for x in ("REAL", "FLOAT", "DOUBLE", "DECIMAL", "NUMERIC")):
        return "REAL"
    if "BLOB" in t or "BYTEA" in t or "BINARY" in t:
        return "BLOB"
    return "TEXT"


def _row_to_strings(row: Any, columns: List[str]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    mapping = row._mapping if hasattr(row, "_mapping") else dict(zip(columns, row))
    for name in columns:
        val = mapping[name]
        out[name] = "" if val is None else str(val)
    return out


def export_external_bundle(
    uri: str,
    *,
    tables: Optional[List[str]] = None,
    schema: Optional[str] = None,
    include_procedures: bool = True,
    include_triggers: bool = True,
    row_limit_per_table: Optional[int] = None,
) -> Dict[str, Any]:
    """Connect via SQLAlchemy and build an AstralDB import bundle."""
    create_engine, inspect, text, Engine = _require_sqlalchemy()
    profile = parse_database_uri(uri)
    engine: Engine = create_engine(profile.dsn, pool_pre_ping=True)
    insp = inspect(engine)
    out_tables: Dict[str, Any] = {}
    procedures: List[ProcedureSpec] = []
    triggers: List[TriggerSpec] = []

    try:
        table_names = insp.get_table_names(schema=schema)
        if tables:
            wanted = {t.strip() for t in tables if t.strip()}
            table_names = [n for n in table_names if n in wanted]

        with engine.connect() as conn:
            for table in table_names:
                cols_meta = insp.get_columns(table, schema=schema)
                pk_cols = {c["name"] for c in insp.get_pk_constraint(table, schema=schema).get("constrained_columns") or []}
                schema_cols: List[Dict[str, Any]] = []
                col_names: List[str] = []
                for col in cols_meta:
                    name = str(col["name"])
                    col_names.append(name)
                    schema_cols.append(
                        {
                            "name": name,
                            "sqlType": _sql_type_to_astral(str(col.get("type"))),
                            "pk": name in pk_cols,
                            "unique": False,
                            "notNull": col.get("nullable") is False,
                        }
                    )
                quoted = table
                if schema:
                    quoted = f'"{schema}"."{table}"' if profile.engine == "postgres" else f"{schema}.{table}"
                sql = f"SELECT * FROM {quoted}"
                if row_limit_per_table is not None:
                    if profile.engine in ("postgres", "mysql", "oracle", "mssql"):
                        sql += f" LIMIT {int(row_limit_per_table)}"
                    elif profile.engine == "mssql":
                        sql = f"SELECT TOP {int(row_limit_per_table)} * FROM {quoted}"
                rows: List[Dict[str, str]] = []
                result = conn.execute(text(sql))
                for row in result:
                    rows.append(_row_to_strings(row, col_names))
                    if row_limit_per_table is not None and len(rows) >= row_limit_per_table:
                        break
                out_tables[table] = {"schema": schema_cols, "rows": rows}

            if include_procedures:
                procedures = _export_procedures(conn, profile.engine, schema=schema)
            if include_triggers:
                triggers = _export_triggers(conn, profile.engine, schema=schema, insp=insp)
    finally:
        engine.dispose()

    bundle: Dict[str, Any] = {
        "bundleVersion": BUNDLE_VERSION,
        "kind": BUNDLE_KIND,
        "source": profile.to_dict(),
        "tables": out_tables,
    }
    if procedures:
        bundle["procedures"] = [p.to_dict() for p in procedures]
    if triggers:
        bundle["triggers"] = [t.to_dict() for t in triggers]
    return bundle


def _export_procedures(conn, engine: str, *, schema: Optional[str]) -> List[ProcedureSpec]:
    specs: List[ProcedureSpec] = []
    from sqlalchemy import text

    if engine == "postgres":
        q = """
        SELECT p.proname AS name, pg_get_functiondef(p.oid) AS def
        FROM pg_proc p
        JOIN pg_namespace n ON n.oid = p.pronamespace
        WHERE n.nspname = COALESCE(:schema, 'public')
          AND p.prokind = 'p'
        ORDER BY 1
        """
        try:
            for row in conn.execute(text(q), {"schema": schema or "public"}):
                name = str(row[0])
                body = str(row[1] or "")
                if body:
                    specs.append(ProcedureSpec(name=name, sql=body, dialect="plpgsql", source=engine))
        except Exception:
            pass
    elif engine == "mysql":
        q = """
        SELECT ROUTINE_NAME, ROUTINE_DEFINITION, ROUTINE_TYPE
        FROM information_schema.ROUTINES
        WHERE ROUTINE_SCHEMA = COALESCE(:schema, DATABASE())
          AND ROUTINE_TYPE = 'PROCEDURE'
        """
        try:
            for row in conn.execute(text(q), {"schema": schema}):
                name = str(row[0])
                body = str(row[1] or "")
                if body:
                    specs.append(
                        ProcedureSpec(
                            name=name,
                            sql=f"CREATE PROCEDURE {name} AS BEGIN {body} END;",
                            dialect="plsql",
                            source=engine,
                        )
                    )
        except Exception:
            pass
    elif engine == "sqlite":
        q = "SELECT name, sql FROM sqlite_master WHERE type='trigger'"
        # sqlite has no procedures; skip
        del q
    elif engine == "oracle":
        q = """
        SELECT object_name, DBMS_METADATA.GET_DDL('PROCEDURE', object_name, owner)
        FROM all_procedures
        WHERE owner = UPPER(COALESCE(:schema, USER))
          AND object_type = 'PROCEDURE'
        """
        try:
            for row in conn.execute(text(q), {"schema": schema}):
                specs.append(ProcedureSpec(name=str(row[0]), sql=str(row[1]), dialect="plsql", source=engine))
        except Exception:
            pass
    return specs


def _export_triggers(conn, engine: str, *, schema: Optional[str], insp) -> List[TriggerSpec]:
    specs: List[TriggerSpec] = []
    from sqlalchemy import text

    if engine == "postgres":
        q = """
        SELECT tgname, pg_get_triggerdef(oid, true)
        FROM pg_trigger
        WHERE NOT tgisinternal
        ORDER BY 1
        """
        try:
            for row in conn.execute(text(q)):
                specs.append(TriggerSpec(name=str(row[0]), sql=str(row[1])))
        except Exception:
            pass
    elif engine == "mysql":
        q = """
        SELECT TRIGGER_NAME, EVENT_MANIPULATION, EVENT_OBJECT_TABLE, ACTION_TIMING, ACTION_STATEMENT
        FROM information_schema.TRIGGERS
        WHERE TRIGGER_SCHEMA = COALESCE(:schema, DATABASE())
        """
        try:
            for row in conn.execute(text(q), {"schema": schema}):
                name, event, table, timing, action = row
                sql = (
                    f"CREATE TRIGGER {name} {timing} {event} ON {table} "
                    f"FOR EACH ROW {action};"
                )
                specs.append(
                    TriggerSpec(
                        name=str(name),
                        sql=sql,
                        table=str(table),
                        timing=str(timing),
                        event=str(event),
                    )
                )
        except Exception:
            pass
    elif engine == "sqlite":
        for row in conn.execute(text("SELECT name, sql FROM sqlite_master WHERE type='trigger' AND sql IS NOT NULL")):
            specs.append(TriggerSpec(name=str(row[0]), sql=str(row[1])))
    return specs


@dataclass
class MigrationSourceInfo:
    kind: str
    engine: str = ""
    notes: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return {"kind": self.kind, "engine": self.engine, "notes": list(self.notes)}


def describe_external_source(uri: str) -> MigrationSourceInfo:
    profile = parse_database_uri(uri)
    notes = [
        f"Engine: {profile.engine}",
        "Requires SQLAlchemy and a database driver.",
        _MIGRATE_EXTRA_HINT,
    ]
    if profile.engine == "athena":
        notes.append("AWS Athena: use awsathena+rest://... URI; export may be read-heavy.")
    if profile.engine == "postgres" and "redshift" in uri.lower():
        notes.append("Amazon Redshift uses the PostgreSQL wire protocol (postgresql+psycopg2://...).")
    return MigrationSourceInfo(kind="external", engine=profile.engine, notes=notes)
