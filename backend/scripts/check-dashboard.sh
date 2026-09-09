#!/usr/bin/env bash
#
# Static checks on frontend/index.html.
#
# The dashboard is one dependency-free file with no build step and no type
# checker, which is a deliberate trade -- it ships inside the server binary, so
# every dependency would have to be vendored into the executable. The cost of
# that trade is that a typo in an element id fails silently in a browser: the
# panel just never populates, and nothing anywhere reports an error.
#
# These checks buy back the part that matters:
#
#   1. every element the script looks up actually exists in the markup
#   2. the deploy-time injection placeholder is present exactly once and
#      byte-identical to what frontend/build.sh substitutes
#   3. no external resource sneaked in -- a CDN link would break both the
#      embedded copy (no network from inside a container) and the artifact
#      Content-Security-Policy
#   4. the script actually parses
#
# Check 4 exists because checks 1-3 did not catch the worst outage this file
# has had. A string literal was written across two lines, which is a parse
# error for the entire <script> block: every handler failed to attach, the
# sign-in dialog never opened, and the page rendered perfectly while doing
# nothing at all. Greps cannot see that. A parser can, so one is used when a
# JavaScript engine is available.
#
#   bash scripts/check-dashboard.sh
#
set -uo pipefail

backend_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dashboard="$backend_root/../frontend/index.html"

if [ ! -f "$dashboard" ]; then
  echo "check-dashboard: $dashboard not found" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "check-dashboard: python3 is required" >&2
  exit 1
fi

python3 - "$dashboard" <<'PY'
import re, sys

path = sys.argv[1]
with open(path, encoding="utf-8") as handle:
    html = handle.read()

failures = []

def check(label, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + label + (("\n       " + detail) if detail and not ok else ""))
    if not ok:
        failures.append(label)

ids = set(re.findall(r'\bid="([^"]+)"', html))

# $("name") is the only way the script reaches the DOM by id, apart from the
# three sparkline ids passed to drawSpark() as literals -- collected here too.
refs = set(re.findall(r'\$\("([^"]+)"\)', html))
refs |= set(re.findall(r'drawSpark\("([^"]+)"', html))

missing = sorted(refs - ids)
check("every element the script looks up exists", not missing,
      "no such id: " + ", ".join(missing))

placeholder = 'window.BOURSE_API_BASE = "";'
count = html.count(placeholder)
check("deploy injection placeholder present exactly once", count == 1,
      f"found {count} occurrences of: {placeholder}")

# A remote asset breaks the embedded copy (a container has no outbound network
# in most deployments) long before anyone notices it in a browser.
external = re.findall(r'(?:src|href)="(https?:|//)[^"]*"', html)
check("no external resources", not external,
      "external reference(s): " + ", ".join(str(e) for e in external))

# Escaping. Whether a *particular* interpolation needs esc() is not decidable
# by regex -- `${k}` over a hardcoded label array and `${k}` over a key name
# from the server look identical -- and a check tuned until it stops firing is
# worse than no check, because it reads as coverage. So this asserts the things
# that are decidable: that the escaper is correct and complete, and that none
# of the sinks which bypass it are used anywhere. Whether each interpolation
# reaches for it stays a review question.
escaper = re.search(r'const esc = .*?;', html, re.S)
check("esc() is defined", escaper is not None)
if escaper:
    body = escaper.group(0)
    covered = [c for c in ("&", "<", ">", '"', "'") if c in body]
    check("esc() covers all five HTML-significant characters", len(covered) == 5,
          "covers only: " + " ".join(covered))

# Sinks that execute strings, or that bypass the escaper entirely.
for sink in ("eval(", "document.write", "new Function(", ".outerHTML", "insertAdjacentHTML"):
    check(f"no {sink}", sink not in html)

# `innerHTML +=` in a poll loop appends on every tick instead of replacing,
# which grows the DOM without bound until the tab dies.
check("no innerHTML += in a rendering path", "innerHTML +=" not in html)

anchors = ["renderBook", "renderTape", "renderStats", "renderResultSet",
           "resolveApiBase", "applyIdentity", "drawSpark"]
absent = [name for name in anchors if f"function {name}" not in html]
check("all render entry points defined", not absent,
      "missing: " + ", ".join(absent))

print()
print(f"  {len(refs)} element references checked, {len(ids)} ids declared")
sys.exit(1 if failures else 0)
PY
status=$?

# ---------------------------------------------------------------------------
# Does the script parse?
#
# Handed to a real JavaScript engine rather than checked with a pattern. The
# fault this exists to catch was a string literal written across two lines,
# which is a parse error for the entire <script> block: every handler failed to
# attach, the sign-in dialog never opened, and the page rendered perfectly
# while doing nothing at all. All eight verify-all stages were green through
# it, because every check above this one is a grep, and a grep cannot tell a
# parse error from prose.
#
# A hand-written scanner was tried first and produced false positives on the
# regex literals in esc(); lexing JavaScript correctly is its own project, and
# a check that cries wolf gets ignored. node is on every CI runner and on most
# developer machines. Where it is missing this reports "skip" rather than
# printing a pass it did not earn.
# ---------------------------------------------------------------------------
echo
# `node` on a Linux box, `node.exe` when this is WSL borrowing the Windows
# install. Either is a real JavaScript engine, which is the whole requirement.
node_bin=""
if command -v node >/dev/null 2>&1; then
  node_bin="node"
elif command -v node.exe >/dev/null 2>&1; then
  node_bin="node.exe"
fi

if [ -n "$node_bin" ]; then
  # node --check refuses a file with no .js extension, so the extracted script
  # gets a real name. It is written beside the dashboard rather than in /tmp:
  # under Git Bash on Windows, python3 and node resolve a /tmp path to two
  # different places and node reports the file as missing.
  script_js="$(dirname "$dashboard")/.dashboard-parse-check.js"
  python3 - "$dashboard" "$script_js" <<'PY'
import re, sys

with open(sys.argv[1], encoding="utf-8") as handle:
    html = handle.read()

# Inline blocks only. There are no src= scripts -- check 3 above enforces that
# -- so this is the whole of the page's behaviour.
blocks = re.findall(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", html, re.S)
with open(sys.argv[2], "w", encoding="utf-8") as out:
    out.write("\n".join(blocks))
PY
  # node.exe cannot read a /mnt/c path; wslpath gives it the Windows form.
  node_arg="$script_js"
  if [ "$node_bin" = "node.exe" ] && command -v wslpath >/dev/null 2>&1; then
    node_arg="$(wslpath -w "$script_js")"
  fi

  if "$node_bin" --check "$node_arg" 2>"$script_js.err"; then
    echo "  ok   the dashboard script parses"
  else
    echo "  FAIL the dashboard script does not parse"
    sed 's/^/       /' "$script_js.err"
    echo "       A parse error here disables every handler on the page."
    status=1
  fi
  rm -f "$script_js" "$script_js.err"
else
  echo "  skip node not found; the script was not parsed"
  echo "       CI runs this check; install node to run it here too."
fi

echo
echo "==================================="
if [ "$status" -eq 0 ]; then
  echo "  PASS"
else
  echo "  FAIL"
fi
echo "==================================="
exit "$status"
