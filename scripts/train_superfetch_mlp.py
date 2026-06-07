#!/usr/bin/env python3
"""
Train Superfetch prefetch MLP from example SQL workloads and emit SuperfetchWeights.inc.

  python scripts/train_superfetch_mlp.py
  python scripts/train_superfetch_mlp.py --examples examples --epochs 300 --report build/superfetch_dataset.jsonl
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO / "scripts") not in sys.path:
    sys.path.insert(0, str(_REPO / "scripts"))

from superfetch_query_dataset import (  # noqa: E402
    K_IN,
    K_OUT,
    TrainingSample,
    collect_samples,
    iter_example_sql_paths,
    synthetic_sweep_samples,
    write_dataset_report,
)


def main() -> None:
    ap = argparse.ArgumentParser(description="Train Superfetch MLP from example SQL analysis")
    ap.add_argument(
        "--examples",
        type=Path,
        action="append",
        default=None,
        help="Directory of .sql examples (default: <repo>/examples)",
    )
    ap.add_argument("--output", type=Path, default=Path("sources/Database/Storage/SuperfetchWeights.inc"))
    ap.add_argument("--report", type=Path, default=None, help="Write JSONL dataset + summary")
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--lr", type=float, default=1e-2)
    ap.add_argument("--val-frac", type=float, default=0.15)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--no-synthetic", action="store_true", help="Only use SQL-derived samples")
    ap.add_argument("--no-examples", action="store_true", help="Synthetic grid only")
    args = ap.parse_args()

    random.seed(args.seed)

    example_dirs = args.examples if args.examples else list(iter_example_sql_paths(_REPO))
    if args.no_examples:
        samples = [] if args.no_synthetic else synthetic_sweep_samples()
        analyses = []
    else:
        samples, analyses = collect_samples(
            example_dirs,
            include_synthetic=not args.no_synthetic,
        )

    if not samples:
        print("No training samples; writing default weights.", file=sys.stderr)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(_default_inc(), encoding="utf-8")
        return

    if args.report:
        write_dataset_report(args.report, samples, analyses)
        print(f"Dataset report: {args.report} ({len(samples)} samples)")

    _print_stats(samples)

    try:
        import torch
        import torch.nn as nn
        from torch.utils.data import DataLoader, TensorDataset
    except ImportError:
        print("torch not installed; writing default inc (pip install torch)", file=sys.stderr)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(_default_inc(), encoding="utf-8")
        return

    xs, ys = _to_tensors(samples, torch)
    n = xs.shape[0]
    perm = torch.randperm(n)
    val_n = max(1, int(n * args.val_frac))
    val_idx = perm[:val_n]
    train_idx = perm[val_n:]

    x_train, y_train = xs[train_idx], ys[train_idx]
    x_val, y_val = xs[val_idx], ys[val_idx]

    train_loader = DataLoader(
        TensorDataset(x_train, y_train),
        batch_size=min(args.batch_size, len(x_train)),
        shuffle=True,
    )

    model = _make_mlp(nn)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    loss_fn = nn.CrossEntropyLoss()

    best_val = float("inf")
    best_state = None

    for epoch in range(args.epochs):
        model.train()
        for xb, yb in train_loader:
            opt.zero_grad()
            loss = loss_fn(model(xb), yb)
            loss.backward()
            opt.step()

        model.eval()
        with torch.no_grad():
            val_loss = loss_fn(model(x_val), y_val).item()
        if val_loss < best_val:
            best_val = val_loss
            best_state = {k: v.detach().clone() for k, v in model.state_dict().items()}
        if (epoch + 1) % max(1, args.epochs // 10) == 0:
            acc = (model(x_val).argmax(1) == y_val).float().mean().item()
            print(f"epoch {epoch + 1}/{args.epochs} val_loss={val_loss:.4f} val_acc={acc:.3f}")

    if best_state:
        model.load_state_dict(best_state)

    w1 = model.net[0].weight.detach().T.contiguous().view(-1).tolist()
    b1 = model.net[0].bias.detach().tolist()
    w2 = model.net[2].weight.detach().T.contiguous().view(-1).tolist()
    b2 = model.net[2].bias.detach().tolist()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(_emit_inc(w1, b1, w2, b2), encoding="utf-8")
    print(f"Wrote {args.output} ({len(samples)} samples, best_val_loss={best_val:.4f})")


def _make_mlp(nn):  # noqa: ANN001
    n_in, n_hidden, n_out = K_IN, 16, K_OUT

    class Mlp(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            self.net = nn.Sequential(
                nn.Linear(n_in, n_hidden),
                nn.ReLU(),
                nn.Linear(n_hidden, n_out),
            )

        def forward(self, x):  # noqa: ANN001
            return self.net(x)

    return Mlp()


def _to_tensors(samples: list[TrainingSample], torch):  # noqa: ANN001
    xs = torch.tensor([s.features for s in samples], dtype=torch.float32)
    ys = torch.tensor([s.label for s in samples], dtype=torch.long)
    return xs, ys


def _print_stats(samples: list[TrainingSample]) -> None:
    from collections import Counter

    by_label = Counter(s.label for s in samples)
    by_wl = Counter(s.workload.name for s in samples)
    n_ex = sum(1 for s in samples if s.source != "synthetic")
    print(f"samples={len(samples)} from_sql={n_ex} synthetic={len(samples) - n_ex}")
    print(f"labels={dict(sorted(by_label.items()))}")
    print(f"workloads={dict(sorted(by_wl.items()))}")


def _fmt(vals: list[float], per_line: int = 8) -> str:
    lines = []
    for i in range(0, len(vals), per_line):
        chunk = ", ".join(f"{v:.6f}f" for v in vals[i : i + per_line])
        lines.append("\t" + chunk + ",")
    return "\n".join(lines)


def _emit_inc(w1: list[float], b1: list[float], w2: list[float], b2: list[float]) -> str:
    return f"""// generated by scripts/train_superfetch_mlp.py
#pragma once

namespace AstralDB::SuperfetchWeights {{

inline constexpr std::size_t kIn = {K_IN};
inline constexpr std::size_t kHidden = 16;
inline constexpr std::size_t kOut = {K_OUT};

alignas(64) inline constexpr float W1[kIn * kHidden] = {{
{_fmt(w1)}
}};

alignas(64) inline constexpr float B1[kHidden] = {{
{_fmt(b1)}
}};

alignas(64) inline constexpr float W2[kHidden * kOut] = {{
{_fmt(w2)}
}};

alignas(64) inline constexpr float B2[kOut] = {{
{_fmt(b2)}
}};

}} // namespace AstralDB::SuperfetchWeights
"""


def _default_inc() -> str:
    return _emit_inc([0.02] * (K_IN * 16), [0.01] * 16, [0.02] * (16 * K_OUT), [0.0] * K_OUT)


if __name__ == "__main__":
    main()
