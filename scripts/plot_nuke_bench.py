#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import platform
import subprocess
from pathlib import Path
from typing import Dict, List


def detect_cpu_name() -> str:
	try:
		out = subprocess.check_output(
			[
				"powershell",
				"-NoProfile",
				"-Command",
				"(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)",
			],
			text=True,
			stderr=subprocess.DEVNULL,
		).strip()
		if out:
			return " ".join(out.split())
	except Exception:
		pass
	fallback = platform.processor().strip()
	return fallback if fallback else "12th Gen Core i5 (detected: unknown)"


def parse_rows(csv_path: Path) -> List[Dict[str, str]]:
	with csv_path.open("r", encoding="utf-8", newline="") as f:
		reader = csv.DictReader(f)
		return list(reader)


def to_float(value: str, default: float = math.nan) -> float:
	try:
		return float(value)
	except Exception:
		return default


def build_plot(rows: List[Dict[str, str]], cpu_name: str, output_path: Path, title_prefix: str) -> None:
	import matplotlib.pyplot as plt

	if not rows:
		raise ValueError("CSV has no benchmark rows to plot.")

	labels: List[str] = []
	avg_seconds: List[float] = []
	speedups: List[float] = []
	colors: List[str] = []

	for row in rows:
		profile = (row.get("Profile") or "").strip()
		mr = (row.get("MR") or "").strip()
		kc = (row.get("KC") or "").strip()
		avg = to_float(row.get("AvgSeconds", ""))
		spd = to_float(row.get("SpeedupVsBaseline", ""), default=1.0)
		if math.isnan(avg):
			continue

		if profile == "baseline":
			label = "baseline"
			colors.append("#888888")
		else:
			label = f"MR={mr} KC={kc}"
			colors.append("#4C78A8")
		labels.append(label)
		avg_seconds.append(avg)
		speedups.append(spd)

	if not labels:
		raise ValueError("No valid AvgSeconds values found in CSV.")

	fig, ax = plt.subplots(figsize=(13, 7))
	x = list(range(len(labels)))
	bars = ax.bar(x, avg_seconds, color=colors, alpha=0.9)
	ax.set_xticks(x)
	ax.set_xticklabels(labels, rotation=35, ha="right")
	ax.set_ylabel("Average Runtime (seconds)")
	ax.set_xlabel("Kernel Profile")
	ax.grid(axis="y", linestyle="--", alpha=0.25)

	ax2 = ax.twinx()
	ax2.plot(x, speedups, color="#F58518", marker="o", linewidth=2.0, label="Speedup vs baseline")
	ax2.set_ylabel("Speedup (x)")
	ax2.set_ylim(bottom=0)

	for idx, bar in enumerate(bars):
		h = bar.get_height()
		ax.text(
			bar.get_x() + bar.get_width() / 2,
			h,
			f"{h:.3f}s",
			ha="center",
			va="bottom",
			fontsize=9,
		)
		ax2.text(
			x[idx],
			speedups[idx],
			f"{speedups[idx]:.2f}x",
			ha="center",
			va="bottom",
			fontsize=8,
			color="#F58518",
		)

	best_idx = min(range(len(avg_seconds)), key=lambda i: avg_seconds[i])
	best_label = labels[best_idx]
	best_val = avg_seconds[best_idx]
	ax.axhline(best_val, color="#54A24B", linestyle=":", linewidth=1.5)
	ax.text(
		0.01,
		0.98,
		f"Best: {best_label} @ {best_val:.3f}s",
		transform=ax.transAxes,
		ha="left",
		va="top",
		fontsize=10,
		bbox=dict(boxstyle="round,pad=0.25", facecolor="#E8F5E9", edgecolor="#54A24B"),
	)

	title = f"{title_prefix} — {cpu_name}"
	ax.set_title(title)

	lines, labels2 = ax2.get_legend_handles_labels()
	if lines:
		ax2.legend(lines, labels2, loc="upper right")

	fig.tight_layout()
	output_path.parent.mkdir(parents=True, exist_ok=True)
	fig.savefig(output_path, dpi=180)
	plt.close(fig)


def main() -> int:
	parser = argparse.ArgumentParser(description="Plot AstralDB nuke microkernel benchmark results.")
	parser.add_argument(
		"--csv",
		type=Path,
		default=Path("build/nuke_tuning_results.csv"),
		help="Path to tuning CSV (default: build/nuke_tuning_results.csv)",
	)
	parser.add_argument(
		"--out",
		type=Path,
		default=Path("build/nuke_tuning_plot.png"),
		help="Output image path (default: build/nuke_tuning_plot.png)",
	)
	parser.add_argument(
		"--title-prefix",
		type=str,
		default="AstralDB nuke.sql microkernel tuning",
		help="Chart title prefix",
	)
	parser.add_argument(
		"--cpu-name",
		type=str,
		default="",
		help="Optional explicit CPU name override",
	)
	args = parser.parse_args()

	if not args.csv.exists():
		raise FileNotFoundError(f"Benchmark CSV not found: {args.csv}")

	rows = parse_rows(args.csv)
	cpu_name = args.cpu_name.strip() if args.cpu_name.strip() else detect_cpu_name()
	build_plot(rows, cpu_name=cpu_name, output_path=args.out, title_prefix=args.title_prefix)
	print(f"Saved chart: {args.out}")
	print(f"CPU in title: {cpu_name}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
