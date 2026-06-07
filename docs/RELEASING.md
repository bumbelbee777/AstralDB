# Releasing AstralDB

Use this checklist when cutting a `v*` tag (see [`.github/workflows/release.yml`](../.github/workflows/release.yml)).

## 1. Release build and tests

```bash
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ci
ctest --test-dir build-ci -L fast --output-on-failure   # matches CI
./build-ci/run_tests                                     # full suite (update README counts)
```

On Windows, match CI: **Ninja + Clang**, then `build-ci\run_tests.exe`.

Update the README **At a glance** table from the final `run_tests` line (e.g. **222** cases, **1252** assertions). Refresh source stats if the tree grew materially:

```bash
# example: line count under sources/
find sources -name '*.cxx' -o -name '*.hxx' | wc -l
```

Example SQL count: **62** top-level under `examples/` + **33** under `examples/benchmarks/`.

## 2. Benchmark charts

```bash
pip install -r scripts/benchmark-requirements.txt
# Install DuckDB and SQLite on PATH for the cross-engine chart (do not pass --skip-duckdb / --skip-sqlite).
python scripts/benchmark_torture_plot.py --astral build-ci/astraldb --runs 3 --output media/astraldb_bench.png
python scripts/benchmark_nuke_plot.py --astral build-ci/astraldb --runs 3 --output media/nuke_bench.png
python scripts/benchmark_antimatterbomb_plot.py --astral build-ci/astraldb --runs 3 --warmup 1 --output media/antimatterbomb_bench.png
python scripts/benchmark_neutroniumbomb_plot.py --astral build-ci/astraldb --rows 100000 --output media/neutroniumbomb_bench.png
python scripts/benchmark_neutroniumbomb_vs_scylla_plot.py --astral build-ci/astraldb --rows 1000000000 --output media/neutroniumbomb_vs_scylla_bench.png
python scripts/benchmark_neutroniumbomb_scale_sweep.py --astral build-ci/astraldb --output media/neutroniumbomb_scale_sweep.png
python scripts/stress_torture_histogram.py --astral build-ci/astraldb --runs 3 --output media/stress_torture_histogram.png
python scripts/gen_pinn_benchmark_sql.py
python scripts/plot_math_sci_pinn_bench.py --astral build-ci/astraldb --runs 3 --output media/benchmark_math_sci_pinn_plot.png
python scripts/gen_inference_nfp_benchmark_sql.py
python scripts/plot_math_sci_inference_nfp_bench.py --astral build-ci/astraldb --runs 3 --output media/benchmark_math_sci_inference_nfp_plot.png
```

On Windows, use `build-ci\astraldb.exe` and forward slashes or escaped paths as needed.

Copy printed medians into the README benchmark tables, commit `media/*.png`, then tag:

```bash
git tag v2.0
git push origin v2.0
```

Release assets: flat `astraldb-linux-amd64`, `astraldb-macos-arm64`, `astraldb-windows-amd64.exe`, `quasar-*-py3-none-any.whl`, and **`SHA256SUMS`** (from [`.github/scripts/stage-release-assets.sh`](../.github/scripts/stage-release-assets.sh)).

Verify after download:

```bash
sha256sum -c SHA256SUMS
```

Release and nightly workflows install **UPX 4.2.4** from GitHub releases into the runner tool cache (see [`.github/actions/setup-upx`](../.github/actions/setup-upx/action.yml)) and pack Linux + Windows CLI assets with `upx --best --lzma` during staging. macOS builds are not UPX-packed.

Local Windows packing (when `upx.exe` is on PATH):

```powershell
powershell -File scripts/pack_release_binary.ps1 -InputPath build/astraldb.exe
```

Release CI configures with `-DASTRALDB_RELEASE_DIST=ON` for maximum strip/LTO/size tuning.

## Nightly pre-releases

Every push to `main` / `master` runs [`.github/workflows/nightly.yml`](../.github/workflows/nightly.yml):

| Tag | Purpose |
|-----|---------|
| [`nightly`](https://github.com/bumbelbee777/AstralDB/releases/tag/nightly) | Rolling pre-release (same assets + `SHA256SUMS`, refreshed each run) |
| `nightly-YYYYMMDD-<run>` | Immutable git tag pointing at the commit for that nightly |

CI ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) also uploads **per-artifact** `.sha256` sidecars next to each binary and the Quasar wheel for workflow-run artifacts.
