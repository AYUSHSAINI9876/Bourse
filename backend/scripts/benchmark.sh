#!/usr/bin/env bash
#
# Throughput and latency measurement using the stock redis-benchmark, plus the
# server's own p50/p99 instrumentation.
#
#   bash scripts/benchmark.sh [port] [requests] [clients]
#
# Note on interpreting the numbers: a WSL2 loopback run understates native
# Linux substantially, and any figure taken on a laptop under thermal load is
# not comparable to one taken on a server. Record the machine alongside the
# number or the number means nothing.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PORT="${1:-6395}"
REQUESTS="${2:-200000}"
CLIENTS="${3:-50}"
BIN="${BOURSE_BUILD_DIR:-build}/bin/bourse-server"

if [[ ! -x "$BIN" ]]; then
  echo "missing $BIN -- run scripts/build.sh first" >&2
  exit 1
fi
if ! command -v redis-benchmark >/dev/null 2>&1; then
  echo "redis-benchmark not found -- install redis-tools" >&2
  exit 1
fi

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

echo "==> machine"
echo "    cores    : $(nproc)"
echo "    kernel   : $(uname -r)"
echo "    compiler : $(g++ --version | head -n 1)"
echo

echo "==> starting bourse-server on port $PORT"
"$BIN" --port "$PORT" --log-level warn &
SERVER_PID=$!

for _ in $(seq 1 50); do
  [[ "$(redis-cli -p "$PORT" PING 2>/dev/null)" == "PONG" ]] && break
  sleep 0.1
done

echo
echo "==> sequential (no pipelining), $REQUESTS requests, $CLIENTS clients"
redis-benchmark -p "$PORT" -t set,get,incr -n "$REQUESTS" -c "$CLIENTS" -q

echo
echo "==> pipelined (depth 16)"
redis-benchmark -p "$PORT" -t set,get -n "$REQUESTS" -c "$CLIENTS" -P 16 -q

echo
echo "==> server-side command latency (nanoseconds, from the HDR histogram)"
redis-cli -p "$PORT" INFO | grep -E 'command_latency|total_commands' || true

echo
echo "==> keyspace"
redis-cli -p "$PORT" INFO | grep -E '^(keys|used_memory|keyspace_)' || true
