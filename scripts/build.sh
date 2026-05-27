#!/usr/bin/env bash
# Build pptctl for the current host. Picks the right CMake preset based on the
# OS, builds in parallel, runs tests. Usage:
#
#   scripts/build.sh                # release build
#   scripts/build.sh debug          # debug build
#   scripts/build.sh asan           # address+UB sanitizer build
#   scripts/build.sh release --no-test    # skip tests
#
# Requires CMake ≥ 3.24 and Ninja. On macOS: `brew install cmake ninja`.
# On Debian/Ubuntu: `apt install cmake ninja-build`.

set -euo pipefail

BUILD_TYPE="${1:-release}"
RUN_TESTS=1
for arg in "$@"; do
    [ "$arg" = "--no-test" ] && RUN_TESTS=0
done

case "$(uname -s)" in
    Darwin)  PRESET="macos-$BUILD_TYPE" ;;
    Linux)   PRESET="linux-$BUILD_TYPE" ;;
    *)       PRESET="$BUILD_TYPE" ;;
esac

# Sanitizer preset isn't host-specific in name.
if [ "$BUILD_TYPE" = "asan" ]; then
    PRESET="asan"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Check tooling.
command -v cmake >/dev/null 2>&1 || { echo "error: cmake not found" >&2; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "error: ninja not found (brew install ninja / apt install ninja-build)" >&2; exit 1; }

echo "==> Configuring preset: $PRESET"
cmake --preset "$PRESET"

echo "==> Building"
cmake --build --preset "$PRESET" --parallel

if [ "$RUN_TESTS" = "1" ]; then
    echo "==> Testing"
    ctest --test-dir "build/$PRESET" --output-on-failure
fi

BIN="build/$PRESET/pptctl"
if [ -x "$BIN" ]; then
    echo ""
    echo "==> Built: $BIN"
    "$BIN" --version
fi
