#!/usr/bin/env bash
#
# Builds and runs the full test suite under AddressSanitizer+UndefinedBehaviour
# and then under ThreadSanitizer. ASan and TSan cannot be combined, hence two
# separate build trees.
#
# This is the check that backs the "sanitizer clean" claim in the README. CI
# runs exactly this script.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FAILED=0

run_suite() {
  local label="$1" sanitizer="$2" build_dir="$3"
  echo
  echo "==================================================================="
  echo "  $label"
  echo "==================================================================="

  if ! BOURSE_BUILD_DIR="$build_dir" bash scripts/build.sh Debug "$sanitizer" >/dev/null 2>&1; then
    echo "BUILD FAILED for $label"
    BOURSE_BUILD_DIR="$build_dir" bash scripts/build.sh Debug "$sanitizer" 2>&1 | tail -n 30
    FAILED=$((FAILED + 1))
    return
  fi

  if BOURSE_LOG_LEVEL=off "$build_dir/bin/bourse_tests" --gtest_brief=1; then
    echo "PASS: $label"
  else
    echo "FAIL: $label"
    FAILED=$((FAILED + 1))
  fi
}

# halt_on_error keeps a leak from being reported and then silently ignored.
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1"

run_suite "AddressSanitizer + UndefinedBehaviorSanitizer" "address+undefined" "build-asan"
run_suite "ThreadSanitizer" "thread" "build-tsan"

echo
echo "==================================================================="
if [[ $FAILED -eq 0 ]]; then
  echo "  all sanitizer suites clean"
else
  echo "  $FAILED sanitizer suite(s) reported problems"
fi
echo "==================================================================="
exit $FAILED
