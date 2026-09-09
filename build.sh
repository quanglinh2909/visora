#!/usr/bin/env bash
# Build helper for Visora.
#
#   ./build.sh              configure if needed, then build
#   ./build.sh clean        wipe build/ and start over
#   ./build.sh test         build, then run the test suite
#   ./build.sh probe        build, then print this machine's capabilities
#
# Environment:
#   BUILD_TYPE   Debug | RelWithDebInfo | Release   (default: RelWithDebInfo)
#   JOBS         parallel jobs                      (default: nproc)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT}/build"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
JOBS="${JOBS:-$(nproc)}"
MODE="${1:-build}"

case "$MODE" in
    clean) echo ">> removing $BUILD_DIR"; rm -rf "$BUILD_DIR" ;;
    build|test|probe) ;;
    *) echo "unknown mode: $MODE (expected build | clean | test | probe)" >&2; exit 2 ;;
esac

missing=()
for tool in cmake pkg-config c++; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if (( ${#missing[@]} > 0 )); then
    echo "missing build tools: ${missing[*]}" >&2
    echo "  sudo apt-get install -y build-essential cmake pkg-config libopencv-dev" >&2
    exit 1
fi

cmake -B "$BUILD_DIR" -S "$ROOT" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j "$JOBS"

case "$MODE" in
    test)  ctest --test-dir "$BUILD_DIR" --output-on-failure ;;
    probe) "$BUILD_DIR/bin/visora-probe" ;;
esac
