#!/usr/bin/env bash
#
# Configure and build Bourse.
#
#   bash scripts/build.sh                       # RelWithDebInfo, tests on
#   bash scripts/build.sh Debug                 # Debug build
#   bash scripts/build.sh Debug address         # + AddressSanitizer
#   BOURSE_TESTS=OFF bash scripts/build.sh      # library and server only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_TYPE="${1:-RelWithDebInfo}"
SANITIZER="${2:-none}"
BUILD_TESTS="${BOURSE_TESTS:-ON}"
BUILD_DIR="${BOURSE_BUILD_DIR:-build}"

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  GENERATOR_ARGS=(-G Ninja)
fi

echo "==> configuring ($BUILD_TYPE, sanitizer=$SANITIZER, tests=$BUILD_TESTS)"
cmake -B "$BUILD_DIR" -S . \
  "${GENERATOR_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DBOURSE_SANITIZER="$SANITIZER" \
  -DBOURSE_BUILD_TESTS="$BUILD_TESTS"

echo
echo "==> building"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "==> artefacts"
ls -1 "$BUILD_DIR/bin" 2>/dev/null || true
