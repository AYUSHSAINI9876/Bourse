#!/usr/bin/env bash
#
# Durability test. Writes data, kills the server with SIGKILL (no clean
# shutdown, no chance to flush anything the WAL had not already committed),
# restarts, and checks the data came back.
#
# It then corrupts the tail of the log the way a crash mid-write would, and
# checks that recovery truncates the torn record and still starts.
#
#   bash scripts/smoke-persistence.sh [port]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PORT="${1:-6393}"
BIN="${BOURSE_BUILD_DIR:-build}/bin/bourse-server"
DATA_DIR="$(mktemp -d)"

command -v redis-cli >/dev/null 2>&1 || { echo "redis-cli not found" >&2; exit 1; }
[[ -x "$BIN" ]] || { echo "missing $BIN -- run scripts/build.sh first" >&2; exit 1; }

PASS=0
FAIL=0
SERVER_PID=""

cleanup() {
  [[ -n "$SERVER_PID" ]] && kill -9 "$SERVER_PID" 2>/dev/null
  rm -rf "$DATA_DIR"
}
trap cleanup EXIT

check() {
  local label="$1" expected="$2" actual="$3"
  if [[ "$actual" == "$expected" ]]; then
    printf '  ok   %-46s -> %s\n' "$label" "$actual"
    PASS=$((PASS + 1))
  else
    printf '  FAIL %-46s -> got %q, want %q\n' "$label" "$actual" "$expected"
    FAIL=$((FAIL + 1))
  fi
}

r() { redis-cli -p "$PORT" "$@" 2>&1; }

start_server() {
  "$BIN" --port "$PORT" --no-http --log-level warn \
         --appendonly yes --dir "$DATA_DIR" --wal-sync always --save-seconds 0 &
  SERVER_PID=$!
  for _ in $(seq 1 60); do
    [[ "$(redis-cli -p "$PORT" PING 2>/dev/null)" == "PONG" ]] && return 0
    sleep 0.1
  done
  echo "server failed to start" >&2
  return 1
}

hard_kill() {
  # SIGKILL: no destructor runs, no final fsync, no snapshot on the way out.
  # Whatever survives, survives because the WAL already committed it.
  kill -9 "$SERVER_PID" 2>/dev/null
  wait "$SERVER_PID" 2>/dev/null
  SERVER_PID=""
}

echo "==> data directory: $DATA_DIR"
echo
echo "==> run 1: write data"
start_server || exit 1
check "SET survivor"          "OK"  "$(r SET survivor "still here")"
check "SET counter"           "1"   "$(r INCR counter)"
check "INCRBY counter 41"     "42"  "$(r INCRBY counter 41)"
check "RPUSH list"            "3"   "$(r RPUSH mylist a b c)"
check "HSET hash"             "2"   "$(r HSET myhash name ayush role admin)"
check "SADD set"              "3"   "$(r SADD myset x y z)"
check "SET with TTL"          "OK"  "$(r SET temporary value EX 3600)"
# survivor, counter, mylist, myhash, myset, temporary -- the two counter
# commands touch one key, not two.
check "keys before crash"     "6"   "$(r DBSIZE)"

echo
echo "==> SIGKILL (simulating a crash, not a shutdown)"
hard_kill
ls -l "$DATA_DIR" | tail -n +2 | awk '{printf "    %s  %s bytes\n", $NF, $5}'

echo
echo "==> run 2: recover from the WAL alone"
start_server || exit 1
check "string survived"       "still here" "$(r GET survivor)"
check "counter survived"      "42"         "$(r GET counter)"
check "list survived"         "3"          "$(r LLEN mylist)"
check "list order preserved"  "a"          "$(r LRANGE mylist 0 0)"
check "hash survived"         "ayush"      "$(r HGET myhash name)"
check "set survived"          "3"          "$(r SCARD myset)"
check "TTL survived"          "1"          "$([[ "$(r TTL temporary)" -gt 3500 ]] && echo 1 || echo 0)"
check "key count matches"     "6"          "$(r DBSIZE)"

echo
echo "==> run 2: clean shutdown writes a snapshot"
r SET post-recovery "written after restart" >/dev/null
kill -TERM "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=""
if [[ -f "$DATA_DIR/bourse.snapshot" ]]; then
  check "snapshot file exists" "1" "1"
  SNAP_BYTES=$(stat -c%s "$DATA_DIR/bourse.snapshot")
  check "snapshot is non-empty" "1" "$([[ "$SNAP_BYTES" -gt 32 ]] && echo 1 || echo 0)"
  WAL_BYTES=$(stat -c%s "$DATA_DIR/bourse.wal" 2>/dev/null || echo 0)
  check "WAL truncated after snapshot" "0" "$WAL_BYTES"
else
  check "snapshot file exists" "1" "0"
fi

echo
echo "==> run 3: recover from the snapshot"
start_server || exit 1
check "snapshot restored strings" "still here"             "$(r GET survivor)"
check "snapshot restored counter" "42"                     "$(r GET counter)"
check "snapshot restored latest"  "written after restart"  "$(r GET post-recovery)"
check "snapshot key count"        "7"                      "$(r DBSIZE)"

echo
echo "==> torn WAL tail (a crash mid-write)"
r SET before-tear value >/dev/null
hard_kill
# Append a truncated record: a valid-looking header with no payload behind it.
printf 'BOUR\xff\xff\x00\x00\x11\x22\x33\x44partial' >> "$DATA_DIR/bourse.wal"
start_server || exit 1
check "server started despite torn tail" "PONG"   "$(r PING)"
check "committed data still intact"      "value"  "$(r GET before-tear)"
check "pre-crash data still intact"      "42"     "$(r GET counter)"
r SET after-repair ok >/dev/null
check "writes work after repair"         "ok"     "$(r GET after-repair)"

kill -TERM "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=""

echo
echo "-------------------------------------------"
printf 'passed: %d   failed: %d\n' "$PASS" "$FAIL"
echo "-------------------------------------------"
[[ $FAIL -eq 0 ]]
