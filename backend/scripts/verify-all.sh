#!/usr/bin/env bash
#
# Everything, in one command. This is the script to run before claiming the
# project works.
#
#   bash scripts/verify-all.sh            # build, unit tests, all smoke tests
#   BOURSE_SANITIZERS=1 bash scripts/verify-all.sh   # + ASan/UBSan/TSan (slow)
#   BOURSE_BENCH=1 bash scripts/verify-all.sh        # + throughput benchmark
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FAILED=0
declare -a SUMMARY

stage() {
  local label="$1"
  shift
  echo
  echo "==================================================================="
  echo "  $label"
  echo "==================================================================="
  if "$@"; then
    SUMMARY+=("PASS  $label")
  else
    SUMMARY+=("FAIL  $label")
    FAILED=$((FAILED + 1))
  fi
}

stage "build (RelWithDebInfo, tests on)" bash scripts/build.sh

if [[ ! -x build/bin/bourse_tests ]]; then
  echo "build produced no test binary; aborting" >&2
  exit 1
fi

stage "unit and integration tests" env BOURSE_LOG_LEVEL=off ./build/bin/bourse_tests --gtest_brief=1
stage "smoke: keyspace over redis-cli"  bash scripts/smoke-test.sh 6390
stage "smoke: HTTP and matching engine" bash scripts/smoke-exchange.sh 6392 8092
stage "smoke: crash recovery"           bash scripts/smoke-persistence.sh 6393
stage "smoke: split deployment + auth"  bash scripts/smoke-deploy.sh
stage "crypto vs. Python hashlib"       bash scripts/verify-crypto.sh
stage "dashboard static checks"         bash scripts/check-dashboard.sh

if [[ "${BOURSE_SANITIZERS:-0}" == "1" ]]; then
  stage "sanitizers (ASan+UBSan, TSan)" bash scripts/check-sanitizers.sh
fi

if [[ "${BOURSE_BENCH:-0}" == "1" ]]; then
  stage "benchmark" bash scripts/benchmark.sh 6395 100000 50
fi

echo
echo "==================================================================="
echo "  summary"
echo "==================================================================="
for line in "${SUMMARY[@]}"; do
  echo "  $line"
done
echo
if [[ $FAILED -eq 0 ]]; then
  echo "  ALL GREEN"
else
  echo "  $FAILED stage(s) failed"
fi
echo "==================================================================="
exit $FAILED
