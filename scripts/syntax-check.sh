#!/usr/bin/env bash
#
# Fast feedback loop: parses and type-checks every translation unit without
# emitting objects or linking. Roughly 10x quicker than a full CMake build and
# catches everything except link errors.
#
#   bash scripts/syntax-check.sh            # all of src/
#   bash scripts/syntax-check.sh src/net    # just one layer
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TARGET="${1:-src}"

mapfile -t SOURCES < <(find "$TARGET" -name '*.cpp' | sort)

if [[ ${#SOURCES[@]} -eq 0 ]]; then
  echo "no sources under $TARGET"
  exit 0
fi

FLAGS=(
  -std=c++20
  -Iinclude
  -DBOURSE_PLATFORM_LINUX=1
  -DBOURSE_HAVE_EPOLL=1
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
  -fsyntax-only
)

failed=0
for src in "${SOURCES[@]}"; do
  if ! g++ "${FLAGS[@]}" "$src"; then
    echo "FAILED: $src"
    failed=$((failed + 1))
  fi
done

echo
if [[ $failed -eq 0 ]]; then
  echo "syntax OK: ${#SOURCES[@]} translation unit(s)"
else
  echo "syntax FAILED in $failed of ${#SOURCES[@]} translation unit(s)"
  exit 1
fi
