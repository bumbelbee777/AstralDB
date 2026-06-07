#!/usr/bin/env python3
"""
Derive Superfetch MLP training rows from AstralDB example SQL (static analysis).

Feature layout matches AstralDB::Superfetch BuildMlpInput (12 floats).
Policy labels are 0..3 (softmax classes in Superfetch.cxx PolicyFromMlp):
  0 = temporal T0, 1 = NTA, 2 = spatial, 3 = async-friendly large sequential.
"""

from __future__ import annotations

import json
import math
import os
import platform
import re
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Iterable, Iterator

K_IN = 12
K_OUT = 4
MAX_LOOKAHEAD = 32

RE_BULK = re.compile(
    r"\bINSERT\s+INTO\s+(`?)(\w+)\1\s+BULK\s+(\d+)",
    re.IGNORECASE,
)
RE_INSERT_VALUES = re.compile(r"\bINSERT\s+INTO\s+(`?)(\w+)\1\s+VALUES\b", re.IGNORECASE)
RE_CREATE = re.compile(r"\bCREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?(`?)(\w+)\1\b", re.IGNORECASE)
RE_FROM = re.compile(r"\bFROM\s+(`?)(\w+)\1\b", re.IGNORECASE)
RE_JOIN = re.compile(r"\bJOIN\s+(`?)(\w+)\1\b", re.IGNORECASE)
RE_OVER = re.compile(r"\bOVER\s*\(", re.IGNORECASE)
RE_ROWS_FRAME = re.compile(r"\bROWS\s+BETWEEN\b", re.IGNORECASE)
RE_GROUP_OLAP = re.compile(r"\b(GROUP\s+BY|CUBE|ROLLUP|GROUPING\s+SETS)\b", re.IGNORECASE)
RE_MATHSCI = re.compile(
    r"\b(MATH_|PINN_|ST_|VEC_|FFT_|GRAD_|TENSOR_|INFERENCE_|FOKKER_|EMBED_)\w*",
    re.IGNORECASE,
)
RE_VALUES_TUPLE = re.compile(r"\)\s*,\s*\(")


class Workload(IntEnum):
    OLAP_SCAN = 0
    OLAP_WINDOW = 1
    OLTP_ROW = 2
    JOIN_NESTED = 3
    MATHSCI_NUMERIC = 4


@dataclass
class TrainingSample:
    features: list[float]
    label: int
    source: str
    row_count: int
    workload: Workload
    statement: str = ""

    def to_dict(self) -> dict:
        return {
            "source": self.source,
            "row_count": self.row_count,
            "workload": self.workload.name,
            "label": self.label,
            "features": self.features,
            "statement": self.statement[:200],
        }


@dataclass
class SqlFileAnalysis:
    path: str
    table_rows: dict[str, int] = field(default_factory=dict)
    samples: list[TrainingSample] = field(default_factory=list)


def strip_sql_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"--[^\n]*", "", text)
    return text


def split_statements(sql: str) -> list[str]:
    parts: list[str] = []
    buf: list[str] = []
    in_str = False
    quote = ""
    i = 0
    while i < len(sql):
        ch = sql[i]
        if in_str:
            buf.append(ch)
            if ch == quote and (i + 1 >= len(sql) or sql[i + 1] != quote):
                in_str = False
            elif ch == quote and i + 1 < len(sql) and sql[i + 1] == quote:
                buf.append(sql[i + 1])
                i += 1
            i += 1
            continue
        if ch in ("'", '"'):
            in_str = True
            quote = ch
            buf.append(ch)
            i += 1
            continue
        if ch == ";":
            stmt = "".join(buf).strip()
            if stmt:
                parts.append(stmt)
            buf = []
            i += 1
            continue
        buf.append(ch)
        i += 1
    tail = "".join(buf).strip()
    if tail:
        parts.append(tail)
    return parts


def count_values_rows(stmt: str) -> int:
    if "VALUES" not in stmt.upper():
        return 0
    return max(1, len(RE_VALUES_TUPLE.findall(stmt)) + 1)


def referenced_tables(stmt: str) -> set[str]:
    names: set[str] = set()
    for pat in (RE_FROM, RE_JOIN):
        for m in pat.finditer(stmt):
            names.add(m.group(2).lower())
    return names


def classify_select(stmt: str) -> Workload:
    up = stmt.upper()
    if RE_MATHSCI.search(stmt):
        return Workload.MATHSCI_NUMERIC
    if RE_OVER.search(stmt):
        return Workload.OLAP_WINDOW
    if " JOIN " in f" {up} ":
        return Workload.JOIN_NESTED
    if RE_GROUP_OLAP.search(stmt):
        return Workload.OLAP_SCAN
    return Workload.OLTP_ROW


def estimate_scan_rows(stmt: str, table_rows: dict[str, int]) -> int:
    refs = referenced_tables(stmt)
    peak = max(table_rows.values()) if table_rows else 0
    if not refs:
        if RE_MATHSCI.search(stmt) and peak > 0:
            return peak
        return min(peak, 256) if peak else 0
    sizes = [table_rows.get(t, 0) for t in refs]
    base = max(sizes) if sizes else 0
    if base == 0:
        return min(peak, 256) if peak else 256
    up = stmt.upper()
    if " WHERE " in up:
        if "BULK" in up or any(table_rows.get(t, 0) >= 100_000 for t in refs):
            return max(64, int(base * 0.95))
        return max(64, int(base * 0.5))
    return base


def default_lookahead(row_count: int) -> int:
    return min(MAX_LOOKAHEAD, max(1, row_count // 32 + 4))


def host_features() -> tuple[float, float, float]:
    """L3 norm, cores norm, arch norm (best-effort on training host)."""
    cores = os.cpu_count() or 8
    l3_bytes = 8 * 1024 * 1024
    machine = platform.machine().lower()
    arch = 4  # Avx2 default
    if "arm" in machine or "aarch" in machine:
        arch = 6
    l3_norm = l3_bytes / (64.0 * 1024.0 * 1024.0)
    return l3_norm, cores / 64.0, arch / 7.0


def build_feature_vector(
    row_count: int,
    workload: Workload,
    *,
    lookahead: int | None = None,
    ema_norm: float | None = None,
    l3_norm: float | None = None,
    cores_norm: float | None = None,
    arch_norm: float | None = None,
) -> list[float]:
    h_l3, h_cores, h_arch = host_features()
    la = lookahead if lookahead is not None else default_lookahead(row_count)
    if ema_norm is None:
        ema_norm = min(1.0, math.log1p(row_count) / 18.0)
    vec = [0.0] * K_IN
    vec[0] = math.log1p(max(0, row_count))
    vec[1] = float(workload) / 4.0
    vec[2] = la / float(MAX_LOOKAHEAD)
    vec[3] = l3_norm if l3_norm is not None else h_l3
    vec[4] = cores_norm if cores_norm is not None else h_cores
    vec[5] = arch_norm if arch_norm is not None else h_arch
    vec[6] = ema_norm
    vec[7] = 1.0
    return vec


def optimal_policy_label(row_count: int, workload: Workload, stmt: str) -> int:
    """Heuristic target class aligned with Superfetch.cxx policy."""
    if row_count < 64:
        return -1
    up = stmt.upper()
    sequential = 0.9 if " ORDER BY " in up or RE_ROWS_FRAME.search(stmt) else 0.7
    if row_count >= 1_000_000:
        return 1
    if workload == Workload.OLAP_WINDOW and row_count >= 100_000:
        return 1
    if workload == Workload.MATHSCI_NUMERIC and row_count >= 32_768:
        return 2
    if workload == Workload.JOIN_NESTED and row_count >= 100_000:
        return 3
    if row_count >= 100_000 and sequential >= 0.85:
        return 3
    if row_count >= 50_000 and workload in (Workload.OLAP_SCAN, Workload.OLAP_WINDOW):
        return 1
    if row_count >= 4096 and workload == Workload.OLAP_SCAN:
        return 1
    return 0


def analyze_statement(stmt: str, table_rows: dict[str, int], source: str) -> list[TrainingSample]:
    out: list[TrainingSample] = []
    kind = stmt.lstrip().upper().split(maxsplit=1)[0] if stmt.strip() else ""

    if kind == "CREATE":
        m = RE_CREATE.search(stmt)
        if m:
            table_rows.setdefault(m.group(2).lower(), 0)
        return out

    if kind == "INSERT":
        mb = RE_BULK.search(stmt)
        if mb:
            table = mb.group(2).lower()
            n = int(mb.group(3))
            table_rows[table] = table_rows.get(table, 0) + n
            return out
        mv = RE_INSERT_VALUES.search(stmt)
        if mv:
            table = mv.group(2).lower()
            n = count_values_rows(stmt)
            table_rows[table] = table_rows.get(table, 0) + n
        return out

    if kind not in ("SELECT", "WITH"):
        return out

    workload = classify_select(stmt)
    row_count = estimate_scan_rows(stmt, table_rows)
    label = optimal_policy_label(row_count, workload, stmt)
    if label < 0:
        return out
    feats = build_feature_vector(row_count, workload)
    out.append(
        TrainingSample(
            features=feats,
            label=label,
            source=source,
            row_count=row_count,
            workload=workload,
            statement=stmt,
        )
    )
    return out


def prescan_table_rows(text: str) -> dict[str, int]:
    """Seed row counts from all BULK/VALUES inserts before per-statement walks."""
    table_rows: dict[str, int] = {}
    for m in RE_BULK.finditer(text):
        table = m.group(2).lower()
        table_rows[table] = table_rows.get(table, 0) + int(m.group(3))
    for stmt in split_statements(text):
        if stmt.lstrip().upper().startswith("INSERT"):
            mv = RE_INSERT_VALUES.search(stmt)
            if mv:
                table = mv.group(2).lower()
                table_rows[table] = table_rows.get(table, 0) + count_values_rows(stmt)
    return table_rows


def file_row_scale_hint(path: Path, table_rows: dict[str, int]) -> int:
    """Upper bound on rows touched in this file (name + bulk peaks)."""
    peak = max(table_rows.values(), default=0)
    name = path.name.lower()
    if "nuke" in name:
        return max(peak, 10_000_000)
    if "stress" in name or "benchmark" in name or "torture" in name:
        return max(peak, 100_000)
    return peak


def emit_file_peak_samples(
    path: Path,
    table_rows: dict[str, int],
    workloads_seen: set[Workload],
    source: str,
) -> list[TrainingSample]:
    """One sample per workload class at the file's peak row scale (captures BULK-heavy scripts)."""
    peak = file_row_scale_hint(path, table_rows)
    if peak < 4096:
        return []
    out: list[TrainingSample] = []
    wl_set = workloads_seen or {Workload.OLTP_ROW}
    for wl in wl_set:
        stmt = f"/* peak */ SELECT 1 FROM t -- {path.name} {wl.name}"
        label = optimal_policy_label(peak, wl, stmt)
        if label < 0:
            continue
        out.append(
            TrainingSample(
                features=build_feature_vector(peak, wl),
                label=label,
                source=source,
                row_count=peak,
                workload=wl,
                statement=stmt,
            )
        )
    return out


def analyze_sql_file(path: Path) -> SqlFileAnalysis:
    text = strip_sql_comments(path.read_text(encoding="utf-8", errors="replace"))
    analysis = SqlFileAnalysis(path=str(path).replace("\\", "/"))
    table_rows = prescan_table_rows(text)
    workloads_seen: set[Workload] = set()
    for stmt in split_statements(text):
        kind = stmt.lstrip().upper().split(maxsplit=1)[0] if stmt.strip() else ""
        if kind in ("SELECT", "WITH"):
            workloads_seen.add(classify_select(stmt))
        for sample in analyze_statement(stmt, table_rows, analysis.path):
            analysis.samples.append(sample)
    analysis.samples.extend(
        emit_file_peak_samples(path, table_rows, workloads_seen, analysis.path)
    )
    analysis.table_rows = dict(table_rows)
    return analysis


def synthetic_sweep_samples() -> list[TrainingSample]:
    """Grid over row counts and workloads (labels from same heuristics as runtime)."""
    out: list[TrainingSample] = []
    for workload in Workload:
        for exp in range(2, 16):
            row_count = int(10 ** (exp / 2.0))
            if row_count < 64:
                continue
            stmt = f"SYNTHETIC SELECT * FROM t -- {workload.name} {row_count}"
            label = optimal_policy_label(row_count, workload, stmt)
            if label < 0:
                continue
            out.append(
                TrainingSample(
                    features=build_feature_vector(row_count, workload),
                    label=label,
                    source="synthetic",
                    row_count=row_count,
                    workload=workload,
                    statement=stmt,
                )
            )
    return out


def collect_samples(
    examples_dirs: Iterable[Path],
    *,
    include_synthetic: bool = True,
    extra_globs: Iterable[str] = ("**/*.sql",),
) -> tuple[list[TrainingSample], list[SqlFileAnalysis]]:
    all_samples: list[TrainingSample] = []
    analyses: list[SqlFileAnalysis] = []
    seen_paths: set[Path] = set()

    for base in examples_dirs:
        if not base.is_dir():
            continue
        for pattern in extra_globs:
            for path in sorted(base.glob(pattern)):
                if not path.is_file():
                    continue
                rp = path.resolve()
                if rp in seen_paths:
                    continue
                seen_paths.add(rp)
                try:
                    analysis = analyze_sql_file(path)
                except OSError as e:
                    print(f"warn: skip {path}: {e}")
                    continue
                analyses.append(analysis)
                all_samples.extend(analysis.samples)

    if include_synthetic:
        all_samples.extend(synthetic_sweep_samples())

    return all_samples, analyses


def write_dataset_report(path: Path, samples: list[TrainingSample], analyses: list[SqlFileAnalysis]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for s in samples:
            f.write(json.dumps(s.to_dict()) + "\n")
        f.write(
            json.dumps(
                {
                    "summary": {
                        "num_samples": len(samples),
                        "num_files": len(analyses),
                        "label_hist": _label_hist(samples),
                    }
                }
            )
            + "\n"
        )


def _label_hist(samples: list[TrainingSample]) -> dict[str, int]:
    hist: dict[str, int] = {}
    for s in samples:
        key = str(s.label)
        hist[key] = hist.get(key, 0) + 1
    return hist


def iter_example_sql_paths(repo_root: Path) -> Iterator[Path]:
    for sub in (repo_root / "examples",):
        if sub.is_dir():
            yield sub


def main() -> None:
    import argparse

    ap = argparse.ArgumentParser(description="Extract Superfetch training samples from example SQL")
    ap.add_argument("--examples", type=Path, default=Path("examples"))
    ap.add_argument("--report", type=Path, default=None)
    ap.add_argument("--no-synthetic", action="store_true")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parents[1]
    base = args.examples if args.examples.is_absolute() else repo / args.examples
    samples, analyses = collect_samples([base], include_synthetic=not args.no_synthetic)
    print(f"files={len(analyses)} samples={len(samples)}")
    print(f"label_hist={_label_hist(samples)}")
    if args.report:
        write_dataset_report(args.report, samples, analyses)
        print(f"wrote {args.report}")


if __name__ == "__main__":
    main()
