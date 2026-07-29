#!/usr/bin/env bash
#
# End-to-end smoke test: starts a real bourse-server and drives it with the
# stock redis-cli. If this passes, wire compatibility is not a claim -- it is a
# demonstration, using a client nobody in this repo wrote.
#
#   bash scripts/smoke-test.sh [port]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PORT="${1:-6390}"
BIN="${BOURSE_BUILD_DIR:-build}/bin/bourse-server"

if [[ ! -x "$BIN" ]]; then
  echo "missing $BIN -- run scripts/build.sh first" >&2
  exit 1
fi
if ! command -v redis-cli >/dev/null 2>&1; then
  echo "redis-cli not found -- install redis-tools" >&2
  exit 1
fi

PASS=0
FAIL=0

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

check() {
  local label="$1" expected="$2" actual="$3"
  if [[ "$actual" == "$expected" ]]; then
    printf '  ok   %-42s -> %s\n' "$label" "$actual"
    PASS=$((PASS + 1))
  else
    printf '  FAIL %-42s -> got %q, want %q\n' "$label" "$actual" "$expected"
    FAIL=$((FAIL + 1))
  fi
}

r() { redis-cli -p "$PORT" "$@" 2>&1; }

echo "==> starting bourse-server on port $PORT"
"$BIN" --port "$PORT" --log-level warn --maxmemory 64mb &
SERVER_PID=$!

for _ in $(seq 1 50); do
  if [[ "$(redis-cli -p "$PORT" PING 2>/dev/null)" == "PONG" ]]; then break; fi
  sleep 0.1
done

if [[ "$(redis-cli -p "$PORT" PING 2>/dev/null)" != "PONG" ]]; then
  echo "server did not become ready" >&2
  exit 1
fi

echo
echo "==> connectivity"
check "PING"                        "PONG"          "$(r PING)"
check "ECHO hello"                  "hello"         "$(r ECHO hello)"

echo
echo "==> strings"
check "SET greeting hello"          "OK"            "$(r SET greeting hello)"
check "GET greeting"                "hello"         "$(r GET greeting)"
check "GET missing"                 ""              "$(r GET nonexistent-key)"
check "APPEND greeting ' world'"    "11"            "$(r APPEND greeting ' world')"
check "GET greeting (appended)"     "hello world"   "$(r GET greeting)"
check "STRLEN greeting"             "11"            "$(r STRLEN greeting)"
check "EXISTS greeting"             "1"             "$(r EXISTS greeting)"
check "TYPE greeting"               "string"        "$(r TYPE greeting)"
check "DEL greeting"                "1"             "$(r DEL greeting)"
check "EXISTS greeting (deleted)"   "0"             "$(r EXISTS greeting)"

echo
echo "==> counters (integer encoding)"
check "INCR hits"                   "1"             "$(r INCR hits)"
check "INCR hits"                   "2"             "$(r INCR hits)"
check "INCRBY hits 10"              "12"            "$(r INCRBY hits 10)"
check "DECRBY hits 5"               "7"             "$(r DECRBY hits 5)"
check "GET hits"                    "7"             "$(r GET hits)"
r SET notanumber abc >/dev/null
check "INCR on non-numeric"         "ERR value is not an integer or out of range" "$(r INCR notanumber)"

echo
echo "==> TTL"
check "SET s v EX 100"              "OK"            "$(r SET session token EX 100)"
check "TTL session (~100)"          "100"           "$(r TTL session)"
check "PERSIST session"             "1"             "$(r PERSIST session)"
check "TTL session (no expiry)"     "-1"            "$(r TTL session)"
check "TTL missing key"             "-2"            "$(r TTL no-such-key)"
check "SETEX temp 100 v"            "OK"            "$(r SETEX temp 100 value)"
check "PEXPIRE temp 50"             "1"             "$(r PEXPIRE temp 50)"
sleep 0.3
check "GET temp (expired)"          ""              "$(r GET temp)"

echo
echo "==> SET options"
check "SET k v NX (absent)"         "OK"            "$(r SET nxkey first NX)"
check "SET k v NX (present)"        ""              "$(r SET nxkey second NX)"
check "GET nxkey unchanged"         "first"         "$(r GET nxkey)"
check "SET k v XX (present)"        "OK"            "$(r SET nxkey third XX)"
check "GET nxkey updated"           "third"         "$(r GET nxkey)"
check "SET absent XX"               ""              "$(r SET never-seen v XX)"

echo
echo "==> lists"
check "RPUSH queue a b c"           "3"             "$(r RPUSH queue a b c)"
check "LPUSH queue z"               "4"             "$(r LPUSH queue z)"
check "LLEN queue"                  "4"             "$(r LLEN queue)"
check "LRANGE queue 0 -1"           "$(printf 'z\na\nb\nc')" "$(r LRANGE queue 0 -1)"
check "LPOP queue"                  "z"             "$(r LPOP queue)"
check "RPOP queue"                  "c"             "$(r RPOP queue)"
check "LLEN queue (after pops)"     "2"             "$(r LLEN queue)"
check "TYPE queue"                  "list"          "$(r TYPE queue)"
check "GET on a list (WRONGTYPE)"   "WRONGTYPE Operation against a key holding the wrong kind of value" "$(r GET queue)"

echo
echo "==> hashes"
check "HSET user name ayush age 21" "2"             "$(r HSET user name ayush age 21)"
check "HGET user name"              "ayush"         "$(r HGET user name)"
check "HLEN user"                   "2"             "$(r HLEN user)"
check "HDEL user age"               "1"             "$(r HDEL user age)"
check "HLEN user (after del)"       "1"             "$(r HLEN user)"
check "TYPE user"                   "hash"          "$(r TYPE user)"

echo
echo "==> sets"
check "SADD tags a b c"             "3"             "$(r SADD tags a b c)"
check "SADD tags a (duplicate)"     "0"             "$(r SADD tags a)"
check "SCARD tags"                  "3"             "$(r SCARD tags)"
check "SISMEMBER tags b"            "1"             "$(r SISMEMBER tags b)"
check "SISMEMBER tags zzz"          "0"             "$(r SISMEMBER tags zzz)"
check "SMEMBERS tags (sorted)"      "$(printf 'a\nb\nc')" "$(r SMEMBERS tags)"
check "SREM tags b"                 "1"             "$(r SREM tags b)"
check "SCARD tags (after rem)"      "2"             "$(r SCARD tags)"

echo
echo "==> pub/sub"
check "PUBLISH news x (no subs)"    "0"             "$(r PUBLISH news hello)"
check "PUBSUB CHANNELS (empty)"     ""              "$(r PUBSUB CHANNELS)"

echo
echo "==> introspection"
check "DBSIZE > 0"                  "yes"           "$([[ "$(r DBSIZE)" -gt 0 ]] && echo yes || echo no)"
check "INFO has bourse_version"     "yes"           "$(r INFO | grep -q bourse_version && echo yes || echo no)"
check "INFO has maxmemory_policy"   "yes"           "$(r INFO | grep -q 'maxmemory_policy:allkeys-lru' && echo yes || echo no)"
check "COMMAND lists >= 30 verbs"   "yes"           "$([[ "$(r COMMAND | wc -l)" -ge 30 ]] && echo yes || echo no)"
check "unknown verb errors"         "yes"           "$(r NOSUCHCOMMAND 2>&1 | grep -q 'unknown command' && echo yes || echo no)"
check "wrong arity errors"          "yes"           "$(r GET 2>&1 | grep -q 'wrong number of arguments' && echo yes || echo no)"

echo
echo "==> pipelining (1000 commands in one stream)"
PIPE_OUT=$( (for i in $(seq 1 1000); do echo "SET pipe:$i $i"; done) | redis-cli -p "$PORT" --pipe 2>&1 | grep -c 'errors: 0' || true)
check "redis-cli --pipe reports 0 errors" "1" "$PIPE_OUT"
check "pipelined key readable"      "500"           "$(r GET pipe:500)"

echo
echo "==> keyspace scan"
check "KEYS pipe:1 matches"         "pipe:1"        "$(r KEYS 'pipe:1')"
check "FLUSHALL"                    "OK"            "$(r FLUSHALL)"
check "DBSIZE after flush"          "0"             "$(r DBSIZE)"

echo
echo "-------------------------------------------"
printf 'passed: %d   failed: %d\n' "$PASS" "$FAIL"
echo "-------------------------------------------"
[[ $FAIL -eq 0 ]]
