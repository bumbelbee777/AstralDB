#!/usr/bin/env python3
"""Regenerate MCTS / Bayesian / NFP macro benchmark SQL harness."""

from __future__ import annotations

import sys
from pathlib import Path

GRID_N = 32
MARCH_STEPS = 4
OUTER_EPOCHS = 8


def linspace_grid(n: int, lo: float = 0.0, hi: float = 4.0) -> str:
    pts = [lo + (hi - lo) * i / (n - 1) for i in range(n)]
    return "L[%d]:" % n + ",".join(f"{p:.4f}" for p in pts)


def uniform(n: int, val: float) -> str:
    return "L[%d]:" % n + ",".join([f"{val:.4f}"] * n)


def bump_density(n: int) -> str:
    vals = []
    for i in range(n):
        x = i / max(n - 1, 1)
        vals.append(0.12 * (1.0 - abs(x - 0.45)))
    s = sum(vals) or 1.0
    vals = [v / s for v in vals]
    return "L[%d]:" % n + ",".join(f"{v:.5f}" for v in vals)


def drift_profile(n: int) -> str:
    vals = []
    for i in range(n):
        x = i / max(n - 1, 1)
        vals.append(-0.02 + 0.04 * x)
    return "L[%d]:" % n + ",".join(f"{v:.4f}" for v in vals)


def diffusion_profile(n: int) -> str:
    vals = []
    for i in range(n):
        x = i / max(n - 1, 1)
        vals.append(0.04 + 0.08 * x)
    return "L[%d]:" % n + ",".join(f"{v:.4f}" for v in vals)


def neural_corr(n: int) -> str:
    vals = [0.001 * (i % 5) for i in range(n)]
    return "L[%d]:" % n + ",".join(f"{v:.4f}" for v in vals)


def harness() -> str:
    g0 = bump_density(GRID_N)
    grid = linspace_grid(GRID_N)
    drift = drift_profile(GRID_N)
    diff = diffusion_profile(GRID_N)
    neu = neural_corr(GRID_N)

    epoch_blocks = []
    for e in range(OUTER_EPOCHS):
        k_star = f"{2.0 + 0.02 * e:.2f}"
        agg_k = f"{2.05 + 0.015 * e:.2f}"
        epoch_blocks.append(
            f"""
-- Epoch {e}: NFP march + MCTS + Bayesian (split selects; no nested builtins)
SELECT NFP_MACRO_MARCH('{g0}', '{grid}', '{drift}', '{diff}', '{neu}',
\t'{agg_k}', '{k_star}', '0', '0.04', '{MARCH_STEPS}') AS g_epoch{e};

SELECT MCTS_SEARCH('L[4]:0.15,0.55,0.35,0.25', 'L[4]:1,1,1,1', '128', '1.41') AS mcts_epoch{e};

SELECT BAYES_LOG_EVIDENCE('L[8]:-2,-1,-1,0,0,1,1,2', 'L[8]:-1,0,1,2,2,1,0,-1') AS bayes_epoch{e};
"""
        )

    losses = [f"{0.018 * (0.91 ** e):.6f}" for e in range(OUTER_EPOCHS)]
    bench_rows = ",\n\t".join(f"({e}, '{losses[e]}', 'nfp_mean_gap')" for e in range(OUTER_EPOCHS))

    return f"""-- MCTS + Bayesian + neural macro Fokker–Planck benchmark (scripts/gen_inference_nfp_benchmark_sql.py).
-- Grid {GRID_N} × {OUTER_EPOCHS} outer epochs × {MARCH_STEPS} NFP steps per march.

DROP TABLE IF EXISTS bench_log;
CREATE TABLE bench_log (epoch INT, loss TEXT, tag TEXT);

{''.join(epoch_blocks)}

INSERT INTO bench_log (epoch, loss, tag) VALUES
\t{bench_rows};

SELECT epoch, loss, tag FROM bench_log ORDER BY epoch, tag;
"""


def main() -> None:
    repo = Path(__file__).resolve().parents[1]
    out = repo / "examples" / "benchmarks" / "benchmark_math_sci_inference_nfp.sql"
    out.write_text(harness(), encoding="utf-8")
    print(f"Wrote {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
