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

echo
echo "==================================="
if [ "$status" -eq 0 ]; then
  echo "  PASS"
else
  echo "  FAIL"
fi
echo "==================================="
exit "$status"
