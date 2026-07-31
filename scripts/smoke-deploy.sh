#!/usr/bin/env bash
#
# Proves the split deployment works: a dashboard served from one origin talking
# to the server on another, which is exactly the Vercel -> Render arrangement.
#
# Everything here fails silently in a browser -- a missing CORS header shows up
# as an empty page and a console message nobody opens -- so it is checked with
# curl, where a missing header is a failed assertion.
#
#   bash scripts/smoke-deploy.sh
#
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
server_bin="$repo_root/build/bin/bourse-server"
http_port=18080
static_port=13000
static_origin="http://localhost:$static_port"
work_dir="$(mktemp -d)"

passed=0
failed=0

check() {
  local label="$1" expected="$2" actual="$3"
  if [[ "$actual" == *"$expected"* ]]; then
    printf '  ok   %s\n' "$label"
    passed=$((passed + 1))
  else
    printf '  FAIL %s\n       expected to contain: %s\n       got: %s\n' \
      "$label" "$expected" "$actual"
    failed=$((failed + 1))
  fi
}

cleanup() {
  [ -n "${server_pid:-}" ] && kill "$server_pid" 2>/dev/null
  [ -n "${static_pid:-}" ] && kill "$static_pid" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$work_dir"
}
trap cleanup EXIT

if [ ! -x "$server_bin" ]; then
  echo "smoke-deploy: $server_bin not built -- run scripts/build.sh first" >&2
  exit 1
fi

echo "=== Deployment smoke test ==="

# ---------------------------------------------------------------------------
# $PORT, the way every container platform passes its assigned port.
# ---------------------------------------------------------------------------
echo
echo "-- \$PORT is honoured --"
PORT=$http_port "$server_bin" --host 127.0.0.1 --port 16380 --dir "$work_dir/data" \
  --log-level error >"$work_dir/server.log" 2>&1 &
server_pid=$!

for _ in $(seq 1 50); do
  curl -fsS "http://127.0.0.1:$http_port/health" >/dev/null 2>&1 && break
  sleep 0.2
done

if ! kill -0 "$server_pid" 2>/dev/null; then
  echo "smoke-deploy: server exited during startup" >&2
  cat "$work_dir/server.log" >&2
  exit 1
fi

check "server bound the port from \$PORT" \
  '"status":"ok"' "$(curl -fsS "http://127.0.0.1:$http_port/health")"

# A bad $PORT must stop the process rather than fall back to a port the
# platform is not routing to.
bad_port_output="$(PORT=notanumber "$server_bin" --host 127.0.0.1 2>&1)"
check "malformed \$PORT is rejected" "PORT must be 1..65535" "$bad_port_output"

# ---------------------------------------------------------------------------
# CORS: the dashboard's origin differs from the API's on a split deploy.
# ---------------------------------------------------------------------------
echo
echo "-- cross-origin access from the static host --"

preflight="$(curl -fsS -i -X OPTIONS "http://127.0.0.1:$http_port/api/orders" \
  -H "Origin: $static_origin" \
  -H "Access-Control-Request-Method: POST" \
  -H "Access-Control-Request-Headers: Content-Type" 2>&1)"
check "preflight returns 204"                "204"                              "$preflight"
check "preflight allows the origin"          "Access-Control-Allow-Origin: *"   "$preflight"
check "preflight allows POST"                "POST"                             "$preflight"
check "preflight allows Content-Type"        "Content-Type"                     "$preflight"

stats="$(curl -fsS -i "http://127.0.0.1:$http_port/api/stats" -H "Origin: $static_origin")"
check "GET carries the CORS header"          "Access-Control-Allow-Origin: *"   "$stats"
check "GET returns stats"                    '"keys"'                           "$stats"

# Orders are submitted as query parameters, not a JSON body -- see
# scripts/smoke-exchange.sh, which is the contract this mirrors.
order="$(curl -fsS -i -X POST -H "Origin: $static_origin" \
  "http://127.0.0.1:$http_port/api/orders?symbol=AAPL&side=BUY&type=LIMIT&quantity=10&price=150.25")"
check "cross-origin POST is accepted"        '"NEW"'                            "$order"
check "POST carries the CORS header"         "Access-Control-Allow-Origin: *"   "$order"

book="$(curl -fsS "http://127.0.0.1:$http_port/api/book/AAPL" -H "Origin: $static_origin")"
check "the cross-origin order reached the book" '"150.25"'                      "$book"

# ---------------------------------------------------------------------------
# The static bundle: what Vercel actually serves.
# ---------------------------------------------------------------------------
echo
echo "-- static bundle --"

BOURSE_API_BASE="http://127.0.0.1:$http_port" bash "$repo_root/web/build.sh" >/dev/null
check "build.sh stamped the backend URL" \
  "window.BOURSE_API_BASE = \"http://127.0.0.1:$http_port\";" \
  "$(cat "$repo_root/web/dist/index.html")"

if command -v python3 >/dev/null 2>&1; then
  python3 -m http.server "$static_port" --bind 127.0.0.1 \
    --directory "$repo_root/web/dist" >/dev/null 2>&1 &
  static_pid=$!
  for _ in $(seq 1 50); do
    curl -fsS "$static_origin/" >/dev/null 2>&1 && break
    sleep 0.2
  done
  page="$(curl -fsS "$static_origin/")"
  check "static host serves the dashboard"   "<title>Bourse"                    "$page"
  check "served page points at the backend"  "http://127.0.0.1:$http_port"      "$page"
else
  echo "  skip python3 not available; static hosting not exercised"
fi

# The embedded copy must keep working with no base at all -- same binary, same
# file, same-origin fetches. This is the case a deploy change can easily break.
embedded="$(curl -fsS "http://127.0.0.1:$http_port/")"
check "embedded copy still ships"            "<title>Bourse"                    "$embedded"
check "embedded copy defaults to same origin" 'window.BOURSE_API_BASE = "";'    "$embedded"

echo
echo "==================================="
printf '  %d passed, %d failed\n' "$passed" "$failed"
echo "==================================="
[ "$failed" -eq 0 ]
