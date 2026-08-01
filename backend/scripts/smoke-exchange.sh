#!/usr/bin/env bash
#
# End-to-end test of the HTTP/REST layer and the matching engine, driven by
# stock curl and redis-cli. Proves the two transports reach the same engine.
#
#   bash scripts/smoke-exchange.sh [resp-port] [http-port]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RESP_PORT="${1:-6392}"
HTTP_PORT="${2:-8092}"
BIN="${BOURSE_BUILD_DIR:-build}/bin/bourse-server"
BASE="http://127.0.0.1:${HTTP_PORT}"

for tool in curl redis-cli; do
  command -v "$tool" >/dev/null 2>&1 || { echo "$tool not found" >&2; exit 1; }
done
[[ -x "$BIN" ]] || { echo "missing $BIN -- run scripts/build.sh first" >&2; exit 1; }

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
    printf '  ok   %-46s -> %s\n' "$label" "$actual"
    PASS=$((PASS + 1))
  else
    printf '  FAIL %-46s -> got %q, want %q\n' "$label" "$actual" "$expected"
    FAIL=$((FAIL + 1))
  fi
}

contains() {
  local label="$1" needle="$2" haystack="$3"
  if [[ "$haystack" == *"$needle"* ]]; then
    printf '  ok   %-46s -> contains %s\n' "$label" "$needle"
    PASS=$((PASS + 1))
  else
    printf '  FAIL %-46s -> %q lacks %q\n' "$label" "$haystack" "$needle"
    FAIL=$((FAIL + 1))
  fi
}

r() { redis-cli -p "$RESP_PORT" "$@" 2>&1; }
status_of() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

echo "==> starting bourse-server (resp=$RESP_PORT http=$HTTP_PORT)"
"$BIN" --port "$RESP_PORT" --http-port "$HTTP_PORT" --log-level warn &
SERVER_PID=$!

for _ in $(seq 1 60); do
  [[ "$(redis-cli -p "$RESP_PORT" PING 2>/dev/null)" == "PONG" ]] && break
  sleep 0.1
done
[[ "$(redis-cli -p "$RESP_PORT" PING 2>/dev/null)" == "PONG" ]] || { echo "server never became ready" >&2; exit 1; }

echo
echo "==> HTTP basics"
check   "GET /health"                     "200" "$(status_of "$BASE/health")"
contains "GET /health body"               '"status":"ok"' "$(curl -s "$BASE/health")"
check   "GET / (dashboard)"               "200" "$(status_of "$BASE/")"
contains "dashboard is real HTML"         "<title>Bourse" "$(curl -s "$BASE/" | head -c 400)"
check   "GET /metrics"                    "200" "$(status_of "$BASE/metrics")"
contains "metrics are Prometheus format"  "# TYPE" "$(curl -s "$BASE/metrics" | head -c 400)"
check   "unknown path is 404"             "404" "$(status_of "$BASE/no-such-route")"
check   "wrong verb is 405 not 404"       "405" "$(status_of -X DELETE "$BASE/health")"
check   "CORS preflight short-circuits"   "204" "$(status_of -X OPTIONS "$BASE/api/stats")"

echo
echo "==> REST keyspace (same registry as RESP)"
check    "PUT /api/keys/greeting"         '"OK"'    "$(curl -s -X PUT "$BASE/api/keys/greeting" -d 'hello')"
check    "GET /api/keys/greeting"         '"hello"' "$(curl -s "$BASE/api/keys/greeting")"
check    "value visible over RESP"        "hello"   "$(r GET greeting)"
check    "DELETE /api/keys/greeting"      "1"       "$(curl -s -X DELETE "$BASE/api/keys/greeting")"
check    "deleted over RESP too"          "0"       "$(r EXISTS greeting)"
check    "GET missing key is null"        "null"    "$(curl -s "$BASE/api/keys/definitely-absent")"

echo
echo "==> REST command passthrough"
check    "POST /api/command SET"          '"OK"'    "$(curl -s -X POST "$BASE/api/command" -d 'SET rest:1 works')"
check    "POST /api/command GET"          '"works"' "$(curl -s -X POST "$BASE/api/command" -d 'GET rest:1')"
check    "quoted argument survives"       '"hello world"' "$(curl -s -X POST "$BASE/api/command" -d 'SET q "hello world"' >/dev/null; curl -s -X POST "$BASE/api/command" -d 'GET q')"
contains "unknown verb errors as JSON"    '"error"' "$(curl -s -X POST "$BASE/api/command" -d 'NOSUCHVERB')"
check    "unknown verb is HTTP 400"       "400"     "$(status_of -X POST "$BASE/api/command" -d 'NOSUCHVERB')"

echo
echo "==> matching engine over RESP"
check    "resting sell accepted"          "NEW"   "$(r ORDER AAPL SELL LIMIT 10 100.50 | sed -n '4p')"
check    "crossing buy fills"             "FILLED" "$(r ORDER AAPL BUY LIMIT 4 101.00 | sed -n '4p')"
contains "trade printed at resting price" "100.50" "$(r TRADES AAPL)"
contains "book shows remaining 6"         "6"      "$(r BOOK AAPL)"
check    "SYMBOLS lists AAPL"             "AAPL"   "$(r SYMBOLS)"
contains "EXCHANGE reports a trade"       "trades_executed" "$(r EXCHANGE)"

echo
echo "==> matching engine over HTTP"
contains "POST /api/orders rests"         '"NEW"' "$(curl -s -X POST "$BASE/api/orders?symbol=MSFT&side=BUY&type=LIMIT&quantity=7&price=50.25")"
contains "book endpoint has the bid"      '"50.25"' "$(curl -s "$BASE/api/book/MSFT")"
contains "order placed via HTTP is visible over RESP" "50.25" "$(r BOOK MSFT)"
contains "aggressive sell fills via HTTP" '"FILLED"' "$(curl -s -X POST "$BASE/api/orders?symbol=MSFT&side=SELL&type=LIMIT&quantity=7&price=50.00")"
contains "trades endpoint shows the fill" '"quantity":7' "$(curl -s "$BASE/api/trades/MSFT")"

echo
echo "==> order semantics"
r FLUSHALL >/dev/null
check    "FOK with no liquidity cancels"  "CANCELLED" "$(r ORDER TSLA BUY LIMIT 100 10.00 FOK | sed -n '4p')"
r ORDER TSLA SELL LIMIT 5 10.00 >/dev/null
check    "IOC partial fills then cancels" "CANCELLED" "$(r ORDER TSLA BUY LIMIT 20 10.00 IOC | sed -n '4p')"
# Assert against the structured JSON rather than a redis-cli line number: the
# text rendering is for humans and its layout is not a contract.
contains "IOC filled the 5 that existed"  '"quantity":5' "$(curl -s "$BASE/api/trades/TSLA")"
contains "IOC remainder did not rest"     '"bids":[]'    "$(curl -s "$BASE/api/book/TSLA")"
check    "market order never rests"       "CANCELLED" "$(r ORDER TSLA BUY MARKET 3 | sed -n '4p')"
contains "bad side is rejected"           "ERR"       "$(r ORDER TSLA SIDEWAYS LIMIT 1 1.00)"
contains "limit without price rejected"   "ERR"       "$(r ORDER TSLA BUY LIMIT 1)"
check    "cancel of unknown order is 0"   "0"         "$(r CANCEL TSLA 999999)"

echo
echo "==> stats endpoint"
contains "stats expose latency percentiles" '"p99"' "$(curl -s "$BASE/api/stats")"
contains "stats expose engine counters"     '"trades_executed"' "$(curl -s "$BASE/api/stats")"

echo
echo "==> HTTP keep-alive"
# curl writes -w once per transfer, so three requests over one reused
# connection print "1" then "0" then "0". Anything else means the server is
# closing between requests and keep-alive is broken.
KA=$(curl -s -o /dev/null -o /dev/null -o /dev/null -w '%{num_connects}' \
     "$BASE/health" "$BASE/health" "$BASE/health")
check "3 requests, 1 TCP connection" "100" "$KA"

echo
echo "-------------------------------------------"
printf 'passed: %d   failed: %d\n' "$PASS" "$FAIL"
echo "-------------------------------------------"
[[ $FAIL -eq 0 ]]
