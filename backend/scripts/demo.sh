#!/usr/bin/env bash
#
# Guided tour. Starts a server, walks through every layer with real clients,
# and leaves it running so you can open the dashboard.
#
#   bash scripts/demo.sh [resp-port] [http-port]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RESP_PORT="${1:-6380}"
HTTP_PORT="${2:-8080}"
BIN="${BOURSE_BUILD_DIR:-build}/bin/bourse-server"
DATA_DIR="$(mktemp -d)"

[[ -x "$BIN" ]] || { echo "missing $BIN -- run scripts/build.sh first" >&2; exit 1; }

cleanup() {
  [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null
  rm -rf "$DATA_DIR"
}
trap cleanup EXIT

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
run() { printf '  $ %s\n' "$*"; "$@" | sed 's/^/    /'; }

"$BIN" --port "$RESP_PORT" --http-port "$HTTP_PORT" --log-level warn \
       --appendonly yes --dir "$DATA_DIR" --maxmemory 128mb &
SERVER_PID=$!
for _ in $(seq 1 60); do
  [[ "$(redis-cli -p "$RESP_PORT" PING 2>/dev/null)" == "PONG" ]] && break
  sleep 0.1
done

R="redis-cli -p $RESP_PORT"

say "1. It is a Redis server (client: stock redis-cli)"
run $R SET user:1 ayush EX 300
run $R GET user:1
run $R TTL user:1
run $R RPUSH queue a b c
run $R LRANGE queue 0 -1
run $R GET queue

say "2. It is an exchange (price-time priority, real order types)"
run $R ORDER AAPL SELL LIMIT 10 100.50
run $R ORDER AAPL SELL LIMIT 10 101.00
echo "  a buy at 101 crosses the 100.50 resting order and prints at 100.50 --"
echo "  price improvement goes to the side that was patient:"
run $R ORDER AAPL BUY LIMIT 4 101.00
run $R BOOK AAPL
run $R TRADES AAPL

say "3. Fill-or-kill is all-or-nothing"
echo "  1000 shares cannot be filled, so nothing trades at all:"
run $R ORDER AAPL BUY LIMIT 1000 101.00 FOK

say "4. It is a database (lexer, parser, AST, query plan)"
run $R SQL "CREATE TABLE fills (id INTEGER PRIMARY KEY, sym TEXT, qty INTEGER, px REAL)"
run $R SQL "INSERT INTO fills VALUES (1, 'AAPL', 100, 150.25), (2, 'MSFT', 50, 300.10), (3, 'AAPL', 75, 151.0)"
run $R SQL "SELECT sym, qty * 2 AS doubled FROM fills WHERE qty > 60 ORDER BY qty DESC"
run $R EXPLAIN "SELECT * FROM fills WHERE qty > 60 LIMIT 1"
run $R DESCRIBE fills

say "5. It speaks HTTP too -- same command registry, JSON out"
run curl -s "http://127.0.0.1:${HTTP_PORT}/api/book/AAPL"
echo
run curl -s -X POST "http://127.0.0.1:${HTTP_PORT}/api/command" -d 'GET user:1'
echo
run curl -s -X POST "http://127.0.0.1:${HTTP_PORT}/api/sql" -d 'SELECT sym FROM fills ORDER BY id'
echo

say "6. Metrics and introspection"
run $R INFO
run $R EXCHANGE

say "7. Roles are enforced identically over both protocols"
echo "  This server runs without auth, so WHOAMI reports anonymous and"
echo "  everything is permitted. Start it with --auth yes and the same"
echo "  registry check that guards RESP guards the REST API too:"
run $R WHOAMI
cat <<'EOF'

    $ bourse-server --auth yes --admin-user admin --admin-password 'a-strong-one'
    127.0.0.1:6380> GET k
    (error) NOAUTH Authentication required.
    127.0.0.1:6380> AUTH admin a-strong-one
    OK
    127.0.0.1:6380> USER ADD reader a-reader-password viewer
    OK
    127.0.0.1:6380> AUTH reader a-reader-password
    OK
    127.0.0.1:6380> SET k v
    (error) NOPERM this user has no permissions to run the 'SET' command

  Proven end to end, over RESP and HTTP, by scripts/smoke-deploy.sh.
EOF

say "8. It survives SIGKILL"
echo "  writing a key, then killing -9 (no clean shutdown, no flush)..."
$R SET survivor "still here" >/dev/null
kill -9 "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
"$BIN" --port "$RESP_PORT" --http-port "$HTTP_PORT" --log-level warn \
       --appendonly yes --dir "$DATA_DIR" &
SERVER_PID=$!
for _ in $(seq 1 60); do
  [[ "$(redis-cli -p "$RESP_PORT" PING 2>/dev/null)" == "PONG" ]] && break
  sleep 0.1
done
echo "  after restart, recovered from the write-ahead log:"
run $R GET survivor

printf '\n\033[1m== dashboard\033[0m\n'
echo "  http://localhost:${HTTP_PORT}"
echo
echo "  Server still running on RESP :${RESP_PORT} / HTTP :${HTTP_PORT}."
echo "  Press Ctrl-C to stop."
wait "$SERVER_PID"
