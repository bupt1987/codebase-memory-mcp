#!/bin/bash
# package-local.sh — Build and package the local macOS arm64 archive.
#
# Usage:
#   scripts/package-local.sh
#   scripts/package-local.sh --skip-build
#
# This is intentionally scoped to the local packaging convention:
#   bin/codebase-memory-mcp-darwin-arm64.tar.xz

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SKIP_BUILD=false

usage() {
    cat <<'EOF'
package-local.sh — Build and package the local macOS arm64 archive.

Usage:
  scripts/package-local.sh
  scripts/package-local.sh --skip-build

This is intentionally scoped to the local packaging convention:
  bin/codebase-memory-mcp-darwin-arm64.tar.xz
EOF
}

for arg in "$@"; do
    case "$arg" in
        --skip-build)
            SKIP_BUILD=true
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $arg" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "ERROR: local package target is macOS arm64; current OS is $(uname -s)" >&2
    exit 1
fi

for tool in codesign file shasum tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool not found: $tool" >&2
        exit 1
    fi
done

BIN="build/c/codebase-memory-mcp"
OUT_DIR="bin"
PACKAGE="$OUT_DIR/codebase-memory-mcp-darwin-arm64.tar.xz"

if ! $SKIP_BUILD; then
    scripts/build.sh --arch arm64
fi

if [[ ! -x "$BIN" ]]; then
    echo "ERROR: expected binary not found or not executable: $BIN" >&2
    exit 1
fi

BIN_INFO="$(file "$BIN")"
if ! grep -q "Mach-O 64-bit executable arm64" <<<"$BIN_INFO"; then
    echo "ERROR: expected arm64 Mach-O binary, got: $BIN_INFO" >&2
    exit 1
fi

codesign --sign - --force "$BIN"
codesign --verify --verbose "$BIN"

cp LICENSE install.sh build/c/
mkdir -p "$OUT_DIR"
tar -cJf "$PACKAGE" -C build/c codebase-memory-mcp LICENSE install.sh

echo "=== Package contents ==="
tar -tf "$PACKAGE"

echo "=== Package size ==="
ls -lh "$PACKAGE"

echo "=== SHA-256 ==="
shasum -a 256 "$PACKAGE"
