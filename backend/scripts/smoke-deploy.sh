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

backend_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
frontend_dir="$(cd "$backend_root/../frontend" && pwd)"
server_bin="$backend_root/build/bin/bourse-server"
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
  [ -n "${auth_pid:-}" ] && kill "$auth_pid" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$work_dir"
}
trap cleanup EXIT

if [ ! -x "$server_bin" ]; then
  echo "smoke-deploy: $server_bin not built -- run backend/scripts/build.sh first" >&2
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
# SQL over HTTP. The dashboard's workbench is the only client for this and it
# is cross-origin on a split deploy, so it is exercised here rather than in
# smoke-exchange.sh.
# ---------------------------------------------------------------------------
sql_base="http://127.0.0.1:$http_port/api/sql"
curl -fsS -X POST "$sql_base" -d 'CREATE TABLE fills (id INTEGER, sym TEXT, qty INTEGER)' >/dev/null
curl -fsS -X POST "$sql_base" -d "INSERT INTO fills VALUES (1, 'AAPL', 100), (2, 'MSFT', 50)" >/dev/null

select_result="$(curl -fsS -X POST "$sql_base" -H "Origin: $static_origin" \
  -d 'SELECT sym, qty FROM fills WHERE qty > 60 ORDER BY qty DESC')"
check "SQL returns columns"                  '"columns":["sym","qty"]'         "$select_result"
check "SQL returns the matching row"         '"AAPL"'                          "$select_result"
check "SQL filtered the non-matching row"    '"rows":[["AAPL",100]]'           "$select_result"

# The plan travels with every result set. Without it the dashboard cannot show
# a query plan at all, because EXPLAIN is a RESP verb and not SQL the parser
# accepts -- a gap that is invisible until someone clicks the button.
check "SQL result carries the query plan"    '"plan":'                         "$select_result"
check "plan names the scan"                  'SeqScan'                         "$select_result"
check "plan names the filter"                'Filter'                          "$select_result"

check "a non-query reports an empty plan"    '"plan":""' \
  "$(curl -fsS -X POST "$sql_base" -d "INSERT INTO fills VALUES (3, 'TSLA', 7)")"
check "a malformed statement is a 400"       "400" \
  "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$sql_base" -d 'SELCT * FROM fills')"

# ---------------------------------------------------------------------------
# The static bundle: what Vercel actually serves.
# ---------------------------------------------------------------------------
echo
echo "-- static bundle --"

BOURSE_API_BASE="http://127.0.0.1:$http_port" bash "$frontend_dir/build.sh" >/dev/null
check "build.sh stamped the backend URL" \
  "window.BOURSE_API_BASE = \"http://127.0.0.1:$http_port\";" \
  "$(cat "$frontend_dir/dist/index.html")"

if command -v python3 >/dev/null 2>&1; then
  python3 -m http.server "$static_port" --bind 127.0.0.1 \
    --directory "$frontend_dir/dist" >/dev/null 2>&1 &
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

# ---------------------------------------------------------------------------
# Authentication, over the wire, on a second server with auth enforced.
#
# Everything above runs unauthenticated, which is the default and what the
# other smoke suites assume. Auth needs its own server because enabling it
# changes the answer to every request.
# ---------------------------------------------------------------------------
echo
echo "-- authentication --"

auth_port=18081
BOURSE_AUTH=yes BOURSE_ADMIN_USER=root BOURSE_ADMIN_PASSWORD=root-password-1 \
BOURSE_DEMO_USER=guest BOURSE_DEMO_PASSWORD=guest-password-1 \
  "$server_bin" --host 127.0.0.1 --http-port "$auth_port" --port 16381 \
  --dir "$work_dir/auth-data" --auth-iterations 1000 --log-level error \
  >"$work_dir/auth.log" 2>&1 &
auth_pid=$!

for _ in $(seq 1 50); do
  curl -fsS "http://127.0.0.1:$auth_port/health" >/dev/null 2>&1 && break
  sleep 0.2
done

if ! kill -0 "$auth_pid" 2>/dev/null; then
  echo "smoke-deploy: authenticated server exited during startup" >&2
  cat "$work_dir/auth.log" >&2
  exit 1
fi

auth_base="http://127.0.0.1:$auth_port"

# /health stays public so a platform health check does not need credentials.
check "health is public"                  '"status":"ok"' "$(curl -fsS "$auth_base/health")"
check "dashboard is public"               "<title>Bourse" "$(curl -fsS "$auth_base/")"

check "unauthenticated API call is refused" "401" \
  "$(curl -s -o /dev/null -w '%{http_code}' "$auth_base/api/stats")"

# The identity endpoint stays public so the dashboard can discover *that*
# credentials are needed. Answering 401 here would make "auth is on and you are
# signed out" indistinguishable from "this build has no auth", and the page
# would have no way to decide whether to show a login screen.
check "identity endpoint is public"       '"authenticated":false' \
  "$(curl -fsS "$auth_base/api/auth/me")"
check "identity endpoint reports auth is on" '"auth_enabled":true' \
  "$(curl -fsS "$auth_base/api/auth/me")"
check "401 carries a bearer challenge"    "WWW-Authenticate: Bearer" \
  "$(curl -s -i "$auth_base/api/stats" | tr -d '\r')"

check "wrong password is refused"         "401" \
  "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$auth_base/api/auth/login" \
     -H 'Content-Type: application/json' -d '{"username":"root","password":"wrong-password"}')"

login_body="$(curl -fsS -X POST "$auth_base/api/auth/login" \
  -H 'Content-Type: application/json' -d '{"username":"root","password":"root-password-1"}')"
check "login returns a token"             '"token"'          "$login_body"
check "login reports the role"            '"role":"admin"'   "$login_body"

admin_token="$(printf '%s' "$login_body" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
if [ -z "$admin_token" ]; then
  echo "  FAIL could not extract a token from the login response" >&2
  failed=$((failed + 1))
else
  auth_header="Authorization: Bearer $admin_token"

  check "bearer token unlocks the API"    '"keys"' \
    "$(curl -fsS "$auth_base/api/stats" -H "$auth_header")"
  check "identity endpoint reports the user" '"username":"root"' \
    "$(curl -fsS "$auth_base/api/auth/me" -H "$auth_header")"
  check "admin can write"                 "200" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$auth_base/api/command" \
       -H "$auth_header" -d 'SET smoke-key smoke-value')"

  # Cross-origin auth needs Authorization on the CORS allowlist, or the browser
  # strips the header and every authenticated call from the CDN copy fails.
  check "preflight allows Authorization"  "Authorization" \
    "$(curl -fsS -i -X OPTIONS "$auth_base/api/stats" -H "Origin: $static_origin" \
       -H 'Access-Control-Request-Method: GET' -H 'Access-Control-Request-Headers: Authorization')"

  # A viewer must be able to read and must not be able to write. This is the
  # assertion that would catch the registry check being bypassed over HTTP.
  curl -fsS -X POST "$auth_base/api/auth/users" -H "$auth_header" \
    -H 'Content-Type: application/json' \
    -d '{"username":"reader","password":"reader-password","role":"viewer"}' >/dev/null
  reader_token="$(curl -fsS -X POST "$auth_base/api/auth/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"reader","password":"reader-password"}' \
    | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
  reader_header="Authorization: Bearer $reader_token"

  check "viewer can read"                 "200" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$auth_base/api/stats" -H "$reader_header")"
  check "viewer cannot write"             "403" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$auth_base/api/command" \
       -H "$reader_header" -d 'SET nope nope')"
  check "viewer cannot list users"        "403" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$auth_base/api/auth/users" -H "$reader_header")"
  check "viewer cannot FLUSHALL"          "403" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$auth_base/api/command" \
       -H "$reader_header" -d 'FLUSHALL')"

  # Logout must actually revoke: a token that still works after logout is the
  # single most common auth bug.
  curl -fsS -X POST "$auth_base/api/auth/logout" -H "$reader_header" >/dev/null
  check "logout revokes the token"        "401" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$auth_base/api/stats" -H "$reader_header")"

  # The seeded read-only account is what lets a public deployment be explored
  # without publishing admin credentials, so its role has to be exactly right.
  # Unlike an account created at runtime it survives a restart, which is the
  # whole reason a README can advertise it.
  demo_login="$(curl -fsS -X POST "$auth_base/api/auth/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"guest","password":"guest-password-1"}')"
  check "seeded demo account can log in"  '"token"'          "$demo_login"
  check "demo account is a viewer"        '"role":"viewer"'  "$demo_login"

  demo_token="$(printf '%s' "$demo_login" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
  check "demo account can read"           "200" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$auth_base/api/stats" \
       -H "Authorization: Bearer $demo_token")"
  check "demo account cannot write"       "403" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$auth_base/api/command" \
       -H "Authorization: Bearer $demo_token" -d 'SET nope nope')"
  check "demo account cannot FLUSHALL"    "403" \
    "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$auth_base/api/command" \
       -H "Authorization: Bearer $demo_token" -d 'FLUSHALL')"

  check "a forged token is refused"       "401" \
    "$(curl -s -o /dev/null -w '%{http_code}' "$auth_base/api/stats" \
       -H 'Authorization: Bearer 0000000000000000000000000000000000000000000000000000000000000000')"

  # The same policy over RESP, through stock redis-cli. Both protocols share
  # one authorization decision in the command registry; these assertions are
  # what prove that is true on the wire rather than only in a unit test.
  if command -v redis-cli >/dev/null 2>&1; then
    resp() { redis-cli -h 127.0.0.1 -p 16381 --no-auth-warning "$@" 2>&1; }

    check "RESP PING works before AUTH"     "PONG"   "$(resp PING)"
    check "RESP GET is refused before AUTH" "NOAUTH" "$(resp GET anything)"
    check "RESP AUTH with a bad password"   "WRONGPASS" \
      "$(resp -a wrong-password --user root GET anything)"
    check "RESP AUTH unlocks the session"   "OK"     "$(resp --user root -a root-password-1 SET resp-key 1)"
    check "RESP WHOAMI reports the user"    "root"   "$(resp --user root -a root-password-1 WHOAMI)"
    check "RESP viewer cannot write"        "NOPERM" \
      "$(resp --user reader -a reader-password SET nope nope)"
    check "RESP viewer can read"            "1"      "$(resp --user reader -a reader-password GET resp-key)"
  else
    echo "  skip redis-cli not available; RESP auth not exercised"
  fi
fi

kill "$auth_pid" 2>/dev/null
unset auth_pid

echo
echo "==================================="
printf '  %d passed, %d failed\n' "$passed" "$failed"
echo "==================================="
[ "$failed" -eq 0 ]
