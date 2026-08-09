#!/usr/bin/env bash
#
# Cross-checks the hand-written SHA-256, HMAC-SHA256 and PBKDF2 against
# Python's hashlib over randomised inputs.
#
# The unit tests already pin the vectors published with each standard, which
# proves the implementation is right on the cases the standards chose. This
# closes the other half: thousands of random inputs, including the shapes that
# break naive implementations --
#
#   * messages at 55, 56, 63, 64 and 65 bytes, where SHA-256 padding branches
#   * empty messages, empty keys, empty salts
#   * HMAC keys longer than the 64-byte block, which must be hashed down first
#   * derived keys longer than 32 bytes, which need the PBKDF2 block counter
#   * passwords containing NUL bytes and invalid UTF-8 (hex in, hex out)
#
# Agreement with an independent implementation on random input is what turns
# "matches the published vectors" into "matches the reference".
#
#   bash scripts/verify-crypto.sh [case-count]
#
set -uo pipefail

backend_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# $BOURSE_BUILD_DIR matches the other scripts, so an alternate build tree can
# be checked without a second copy of this one. Absolute paths are honoured.
build_dir="${BOURSE_BUILD_DIR:-build}"
case "$build_dir" in
  /*) checker="$build_dir/bin/bourse-crypto-check" ;;
  *) checker="$backend_root/$build_dir/bin/bourse-crypto-check" ;;
esac
cases="${1:-400}"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

if [ ! -x "$checker" ]; then
  echo "verify-crypto: $checker not built -- run scripts/build.sh first" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "verify-crypto: python3 is required as the reference implementation" >&2
  exit 1
fi

echo "=== Cross-checking crypto against Python hashlib ($cases cases) ==="

python3 - "$cases" "$work_dir" <<'PY'
import hashlib, hmac, os, random, sys

count = int(sys.argv[1])
work = sys.argv[2]
random.seed(20260801)  # reproducible: a failure can be re-run identically

requests, expected = [], []

def rand_bytes(n):
    return bytes(random.getrandbits(8) for _ in range(n))

def field(data):
    """Hex, or '-' for empty.

    An empty hex string vanishes in a whitespace-separated line and the next
    field slides into its place, which would silently compare the wrong pair.
    """
    return data.hex() if data else "-"

# Lengths that straddle every SHA-256 padding branch, plus random ones.
lengths = [0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129]
for i in range(count):
    n = lengths[i % len(lengths)] if i < len(lengths) * 3 else random.randint(0, 2000)
    msg = rand_bytes(n)
    requests.append("sha256 " + field(msg))
    expected.append(hashlib.sha256(msg).hexdigest())

# HMAC: keys shorter than, equal to, and longer than the block size.
key_lengths = [0, 1, 32, 63, 64, 65, 100, 131, 200]
for i in range(count):
    key = rand_bytes(key_lengths[i % len(key_lengths)])
    msg = rand_bytes(random.randint(0, 500))
    requests.append(f"hmac {field(key)} {field(msg)}")
    expected.append(hmac.new(key, msg, hashlib.sha256).hexdigest())

# PBKDF2: low iteration counts so this stays fast, but derived lengths that
# span one, two and three output blocks.
for i in range(max(20, count // 8)):
    password = rand_bytes(random.randint(0, 80))
    salt = rand_bytes(random.randint(0, 40))
    iterations = random.choice([1, 2, 7, 64, 333])
    length = random.choice([1, 16, 31, 32, 33, 64, 100])
    requests.append(f"pbkdf2 {field(password)} {field(salt)} {iterations} {length}")
    expected.append(hashlib.pbkdf2_hmac("sha256", password, salt, iterations, length).hex())

with open(os.path.join(work, "requests.txt"), "w") as f:
    f.write("\n".join(requests) + "\n")
with open(os.path.join(work, "expected.txt"), "w") as f:
    f.write("\n".join(expected) + "\n")

print(f"  generated {len(requests)} cases")
PY

if [ ! -s "$work_dir/requests.txt" ]; then
  echo "verify-crypto: failed to generate cases" >&2
  exit 1
fi

if ! "$checker" < "$work_dir/requests.txt" > "$work_dir/actual.txt"; then
  echo "verify-crypto: the checker exited non-zero" >&2
  exit 1
fi

total="$(wc -l < "$work_dir/expected.txt")"
if diff -q "$work_dir/expected.txt" "$work_dir/actual.txt" >/dev/null; then
  echo "  $total/$total digests match Python hashlib"
  echo "==================================="
  echo "  PASS"
  echo "==================================="
  exit 0
fi

echo "  MISMATCH against the reference implementation" >&2
echo "  (line N below is line N of the generated request list)" >&2
diff "$work_dir/expected.txt" "$work_dir/actual.txt" | head -20 >&2
# Keep the inputs: the seed is fixed, but having the exact failing case to hand
# beats regenerating it.
if cp "$work_dir/requests.txt" "$work_dir/expected.txt" "$work_dir/actual.txt" /tmp/ 2>/dev/null; then
  echo "  inputs and outputs copied to /tmp for inspection" >&2
fi
echo "===================================" >&2
echo "  FAIL" >&2
echo "===================================" >&2
exit 1
