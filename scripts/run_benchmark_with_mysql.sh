#!/usr/bin/env bash
# Starts MySQL in Docker, waits until healthy, runs benchmark_torture_plot.py, then removes the container.
# Requires: Docker with Compose v2 (`docker compose`).
#
# Usage:
#   ./scripts/run_benchmark_with_mysql.sh
#   ./scripts/run_benchmark_with_mysql.sh --no-fetch --runs 1 --scale 0.001 --output plot.png
#   ./scripts/run_benchmark_with_mysql.sh --keep-container   # leave MySQL running after benchmark

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPOSE="$ROOT/scripts/docker/mysql-benchmark-compose.yml"
PROJECT="astraldb-bench-mysql"

# If docker info returns HTTP 500 for every API version, restart Docker Desktop or run
# `wsl --shutdown` (WSL2). Optional: export DOCKER_API_VERSION=1.43 only for the rare v1.24 client bug.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPOSE="$ROOT/scripts/docker/mysql-benchmark-compose.yml"
PROJECT="astraldb-bench-mysql"

if [[ -n "${DOCKER_API_VERSION:-}" ]]; then
	echo "Using DOCKER_API_VERSION=${DOCKER_API_VERSION} (from environment)."
fi

if ! command -v docker >/dev/null 2>&1; then
	echo "Docker is not on PATH." >&2
	exit 1
fi
if ! docker compose version >/dev/null 2>&1; then
	echo "'docker compose' is not available. Install Docker Compose v2." >&2
	exit 1
fi

NO_FETCH=()
KEEP=0
USER_ARGS=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--no-fetch)
			NO_FETCH=(--no-fetch)
			shift
			;;
		--keep-container)
			KEEP=1
			shift
			;;
		*)
			USER_ARGS+=("$1")
			shift
			;;
	esac
done

cleanup() {
	if [[ "$KEEP" -eq 0 ]]; then
		echo "Stopping MySQL container ..."
		docker compose -f "$COMPOSE" -p "$PROJECT" down -v >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

deadline=$(( $(date +%s) + 180 ))
until docker info >/dev/null 2>&1; do
	now=$(date +%s)
	if [ "$now" -ge "$deadline" ]; then
		echo "Docker engine did not become ready in time (often HTTP 500 = wedged engine)." >&2
		echo "Try: quit Docker Desktop, wsl --shutdown, start Docker again." >&2
		exit 1
	fi
	echo "Waiting for Docker engine ..."
	sleep 2
done

echo "Starting MySQL ($PROJECT) ..."
docker compose -f "$COMPOSE" -p "$PROJECT" up -d --wait

echo "Running benchmark_torture_plot.py ..."
python3 "$ROOT/scripts/benchmark_torture_plot.py" \
	--repo-root "$ROOT" \
	--mysql-host 127.0.0.1 \
	--mysql-port 13306 \
	--mysql-user root \
	--mysql-password astraldb_bench_root \
	--mysql-database astraldb_bench \
	"${NO_FETCH[@]}" \
	"${USER_ARGS[@]}"

if [[ "$KEEP" -eq 1 ]]; then
	trap - EXIT
	echo "Leaving container running. Tear down with:"
	echo "  docker compose -f \"$COMPOSE\" -p \"$PROJECT\" down -v"
fi
