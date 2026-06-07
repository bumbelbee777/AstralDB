#!/usr/bin/env python3
"""Run antimatterbomb.sql at reduced scale; report first failure per query block."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
BULK_RX = re.compile(r"BULK\s+\d+", re.IGNORECASE)
QUERY_RX = re.compile(r"^-- Query (\d+):", re.MULTILINE)


def scale_sql(text: str, rows: int) -> str:
    return BULK_RX.sub(f"BULK {rows}", text)


def split_queries(text: str) -> list[tuple[str, str]]:
    parts = QUERY_RX.split(text)
    if len(parts) < 2:
        return [("all", text)]
    header = parts[0]
    out: list[tuple[str, str]] = []
    for i in range(1, len(parts), 2):
        qn = parts[i]
        body = parts[i + 1] if i + 1 < len(parts) else ""
        out.append((f"Q{qn}", header + f"-- Query {qn}:" + body))
    return out


def run_sql(astral: Path, sql: str, opt: str, timeout: int) -> tuple[int, str]:
    with tempfile.NamedTemporaryFile(mode="w", suffix=".sql", delete=False, encoding="utf-8") as tf:
        tf.write(sql)
        path = Path(tf.name)
    try:
        proc = subprocess.run(
            [str(astral), "-m", opt, str(path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        blob = proc.stdout + "\n" + proc.stderr
        return proc.returncode, blob
    finally:
        path.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--astral", type=Path, default=_REPO / "build" / "astraldb.exe")
    ap.add_argument("--rows", type=int, default=50_000)
    ap.add_argument("--opt", default="-O0")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--full", action="store_true", help="Run entire script at once")
    args = ap.parse_args()

    src = (_REPO / "examples" / "antimatterbomb.sql").read_text(encoding="utf-8")
    scaled = scale_sql(src, args.rows)
    astral = args.astral.resolve()
    if not astral.is_file():
        print(f"Missing {astral}", file=sys.stderr)
        return 2

    if args.full:
        rc, blob = run_sql(astral, scaled, args.opt, args.timeout)
        print(f"full script exit={rc}")
        if rc != 0:
            print(blob[-4000:])
        return rc

    blocks = split_queries(scaled)
    setup_only = blocks[0][1].split("-- Query 1:")[0]
    print(f"Setup ({args.rows:,} rows/table)…", flush=True)
    rc, blob = run_sql(astral, setup_only, args.opt, args.timeout)
    print(f"  setup exit={rc}")
    if rc != 0:
        print(blob[-4000:])
        return 1

    cumulative = setup_only
    for name, _ in blocks:
        if name == "all":
            continue
        # append only this query section
        idx = scaled.find(f"-- Query {name[1]}:")
        if idx < 0:
            continue
        nxt = re.search(r"^-- Query \d+:", scaled[idx + 8 :], re.MULTILINE)
        end = idx + 8 + nxt.start() if nxt else len(scaled)
        section = scaled[idx:end]
        cumulative = setup_only + "\n" + section
        print(f"{name}…", flush=True)
        rc, blob = run_sql(astral, cumulative, args.opt, args.timeout)
        print(f"  {name} exit={rc}")
        if rc != 0:
            print(blob[-4000:])
            return 1
    print("All query blocks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
