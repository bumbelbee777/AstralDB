#!/usr/bin/env bash
# Install UPX from GitHub releases into RUNNER_TOOL_CACHE (pair with actions/cache on that path).
set -euo pipefail

VERSION="${1:-4.2.4}"
ROOT="${RUNNER_TOOL_CACHE:-${HOME}/.cache}/upx/${VERSION}"
mkdir -p "$ROOT"

BIN="${ROOT}/upx"
if [[ -x "$BIN" ]]; then
  echo "UPX cache hit: $BIN"
else
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64 | amd64) ARCH=amd64 ;;
    aarch64 | arm64) ARCH=arm64 ;;
  esac

  URL=""
  if [[ "$OS" == "linux" && "$ARCH" == "amd64" ]]; then
    URL="https://github.com/upx/upx/releases/download/v${VERSION}/upx-${VERSION}-amd64_linux.tar.xz"
    curl -fsSL "$URL" | tar -xJ -C "$TMP" --strip-components=1
  elif [[ "$OS" == "darwin" && "$ARCH" == "arm64" ]]; then
    URL="https://github.com/upx/upx/releases/download/v${VERSION}/upx-${VERSION}-arm64_macos.tar.xz"
    curl -fsSL "$URL" | tar -xJ -C "$TMP" --strip-components=1
  elif [[ "$OS" == "darwin" && "$ARCH" == "amd64" ]]; then
    URL="https://github.com/upx/upx/releases/download/v${VERSION}/upx-${VERSION}-amd64_macos.tar.xz"
    curl -fsSL "$URL" | tar -xJ -C "$TMP" --strip-components=1
  else
    echo "setup-upx.sh: unsupported Unix platform ${OS}-${ARCH}" >&2
    exit 1
  fi

  if [[ ! -x "$TMP/upx" ]]; then
    echo "UPX binary missing after extracting ${URL}" >&2
    exit 1
  fi
  install -m755 "$TMP/upx" "$BIN"
  echo "Installed UPX ${VERSION} to $BIN"
fi

if [[ -n "${GITHUB_PATH:-}" ]]; then
  echo "$ROOT" >>"$GITHUB_PATH"
fi
if [[ -n "${GITHUB_ENV:-}" ]]; then
  echo "UPX_BIN=$BIN" >>"$GITHUB_ENV"
  echo "ASTRALDB_UPX_BIN=$BIN" >>"$GITHUB_ENV"
fi

"$BIN" -V 2>/dev/null || "$BIN" --version 2>/dev/null || true
