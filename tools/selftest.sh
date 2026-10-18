#!/usr/bin/env bash
# CI self-test for the Python tools and the C++ binaries in BUILD_DIR.
#  1. The generator is deterministic: regenerating the committed test file is byte-identical.
#  2. itch_stats decodes it with no unknown or malformed messages.
set -euo pipefail
BUILD="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
python3 "$ROOT/tools/gen_itch.py" --messages 40000 --stocks 3 --seed 7 --out "$TMP/s.itch" >/dev/null
cmp "$TMP/s.itch" "$ROOT/tests/data/synth_small.itch"
cmp "$TMP/s.itch.counts" "$ROOT/tests/data/synth_small.itch.counts"
echo "generator deterministic: OK"
"$BUILD/itch_stats" "$ROOT/tests/data/synth_small.itch" --passes 1 >/dev/null
echo "itch_stats on synthetic day: OK"

# 3. Differential test on a larger random day: both C++ books must reproduce the
#    reference book's BBO stream exactly.
python3 "$ROOT/tools/gen_itch.py" --messages 300000 --stocks 50 --seed 2024 --out "$TMP/m.itch" >/dev/null
python3 "$ROOT/tools/ref_book.py" "$TMP/m.itch" "$TMP/m.bbo" >/dev/null
for book in fast baseline; do
  "$BUILD/replay" "$TMP/m.itch" --book "$book" --compare "$TMP/m.bbo" | grep -q "identical"
  echo "replay --book $book matches reference on 300k-message day: OK"
done
