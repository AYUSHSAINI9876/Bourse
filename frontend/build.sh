#!/usr/bin/env bash
#
# Static-deploy build for the dashboard.
#
# There is no build step in the usual sense: the dashboard is one
# dependency-free HTML file, and the server binary embeds that same file. All
# this does is copy it and stamp in the backend URL, so the CDN copy and the
# embedded copy stay byte-identical apart from a single line.
#
# Vercel runs this via the buildCommand in vercel.json, with the project's
# Root Directory set to frontend/. It also runs fine on its own, which is the
# only way to see what will actually ship:
#
#   BOURSE_API_BASE=https://bourse.onrender.com bash frontend/build.sh
#   python3 -m http.server -d frontend/dist 3000
#
set -euo pipefail

frontend_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_file="$frontend_dir/index.html"
output_dir="$frontend_dir/dist"

# Must match frontend/index.html exactly, spacing included.
placeholder='window.BOURSE_API_BASE = "";'

if [ ! -f "$source_file" ]; then
  echo "build: $source_file not found" >&2
  exit 1
fi

if ! grep -qF "$placeholder" "$source_file"; then
  echo "build: the injection point is missing from frontend/index.html." >&2
  echo "build: expected a line containing: $placeholder" >&2
  exit 1
fi

mkdir -p "$output_dir"

# Trailing slash removed here as well as in the page: base + "/api/stats" with
# one already attached gives a double slash, which some proxies answer with a
# redirect that the fetch then drops.
api_base="${BOURSE_API_BASE:-}"
api_base="${api_base%/}"

if [ -z "$api_base" ]; then
  cp "$source_file" "$output_dir/index.html"
  echo "build: BOURSE_API_BASE is not set, so the page will call its own origin."
  echo "build: that origin serves no API, so the dashboard will load and show"
  echo "build: its connection screen until you either set the variable and"
  echo "build: redeploy, or type the backend URL into the field it offers."
  echo "build: wrote $output_dir/index.html"
  exit 0
fi

# Allowlist rather than a blocklist. The value is interpolated into a JS string
# literal, so a quote or backslash getting through would be script injection
# into every visitor's page -- and the person setting this variable in a
# hosting dashboard is not thinking about escaping.
if ! printf '%s' "$api_base" | grep -Eq '^https?://[A-Za-z0-9._~:/-]+$'; then
  echo "build: BOURSE_API_BASE must be a plain http(s) URL, got '$api_base'" >&2
  exit 1
fi

# A page served over HTTPS cannot call an HTTP backend -- the browser blocks it
# as mixed content and the only symptom is a console error most people never
# open. Fail here instead.
case "$api_base" in
  http://localhost*|http://127.0.0.1*) ;;
  http://*)
    echo "build: BOURSE_API_BASE is http://, but this page is served over https." >&2
    echo "build: browsers block that as mixed content. Use an https:// backend." >&2
    exit 1
    ;;
esac

sed "s|$placeholder|window.BOURSE_API_BASE = \"$api_base\";|" \
  "$source_file" > "$output_dir/index.html"

# Verifying rather than trusting sed: if the placeholder in the dashboard is
# ever reworded, the substitution silently does nothing and ships a page
# pointing at the wrong origin. Better a red build than a broken deploy.
if ! grep -qF "window.BOURSE_API_BASE = \"$api_base\";" "$output_dir/index.html"; then
  echo "build: injection failed -- placeholder present but substitution did not apply" >&2
  exit 1
fi

echo "build: wrote $output_dir/index.html -> backend $api_base"
