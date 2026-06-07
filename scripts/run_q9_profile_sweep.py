#!/usr/bin/env python3
"""Profile antimatterbomb Q9 at multiple scales; print ranked region timings."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent


def main() -> int:
    scales = [1_000_000, 10_000_000, 100_000_000]
    if len(sys.argv) > 1:
        scales = [int(x.replace("_", "")) for x in sys.argv[1].split(",")]

    out_root = _REPO / "media" / "q9_profile_sweep"
    out_root.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env.setdefault("ASTRALDB_USE_PRECOMPUTED", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_VM", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_COLUMNAR_COMMIT", "1")
    env.setdefault("ASTRALDB_SEMISTRUCTURED_PARALLEL", "1")

    script = _REPO / "scripts" / "run_antimatterbomb_profile.py"
    summary_all: dict[str, object] = {"scales": {}}

    for rows in scales:
        prof_dir = out_root / f"rows_{rows}"
        summary_path = prof_dir / "summary.json"
        cmd = [
            sys.executable,
            str(script),
            "--queries",
            "9",
            "--rows",
            str(rows),
            "--warmup",
            "1",
            "--repeat",
            "1",
            "--profile-dir",
            str(prof_dir),
            "--summary",
            str(summary_path),
        ]
        print(f"\n=== Q9 profile {rows:,} rows/table ===", flush=True)
        subprocess.run(cmd, check=False, env=env, cwd=_REPO)

        q9 = {}
        if summary_path.is_file():
            data = json.loads(summary_path.read_text(encoding="utf-8"))
            q9 = data.get("queries", {}).get("Q9", {})
        summary_all["scales"][str(rows)] = q9

        prof = q9.get("profile")
        regions: dict[str, float] = {}
        if isinstance(prof, list) and prof:
            entry = prof[0]
            if isinstance(entry, dict):
                rt = entry.get("region_timings")
                if isinstance(rt, dict):
                    for name, ns in rt.items():
                        regions[name] = float(ns) / 1e6
        if regions:
            total = sum(regions.values())
            print(f"execute_ms={q9.get('execute_ms')}  fast_path_flags={q9.get('fast_path_flags')}")
            print(f"region_sum_ms={total:.3f}  (profile regions, may overlap nested scopes)")
            for name, ms in sorted(regions.items(), key=lambda x: -x[1]):
                pct = 100.0 * ms / total if total > 0 else 0.0
                print(f"  {ms:8.3f} ms ({pct:5.1f}%)  {name}")
        else:
            print("No region_timings in profile JSON (set ASTRALDB_PROFILE_OUTPUT via --profile-dir)")

    manifest = out_root / "sweep.json"
    manifest.write_text(json.dumps(summary_all, indent=2), encoding="utf-8")
    print(f"\nWrote {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
