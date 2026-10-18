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

# 4. UDP multicast end to end (loopback): paced replay must reproduce the reference
#    stream exactly; with injected drops the feed must report exactly those gaps.
if "$BUILD/feed" --idle-ms 200 >/dev/null 2>&1; [ $? -ne 1 ]; then
  "$BUILD/feed" --compare "$TMP/m.bbo" --idle-ms 3000 > "$TMP/feed1.out" 2>&1 &
  FEED=$!; sleep 0.5
  "$BUILD/replayer" "$TMP/m.itch" --send --rate ${SELFTEST_RATE:-200000} >/dev/null
  wait $FEED
  grep -q "identical" "$TMP/feed1.out" || { cat "$TMP/feed1.out"; exit 1; }
  echo "multicast feed matches reference on 300k-message day: OK"
  "$BUILD/feed" --idle-ms 3000 > "$TMP/feed2.out" 2>&1 &
  FEED=$!; sleep 0.5
  "$BUILD/replayer" "$TMP/m.itch" --send --rate ${SELFTEST_RATE:-200000} --drop-every 25 > "$TMP/rep2.out"
  wait $FEED || true
  want=$(sed -n 's/.*dropped on purpose holding \([0-9]*\) messages.*/\1/p' "$TMP/rep2.out")
  got=$(sed -n 's/^gaps .*(\([0-9]*\) messages missing)/\1/p' "$TMP/feed2.out")
  [ "$want" = "$got" ] || { echo "injected $want, detected $got"; cat "$TMP/feed2.out"; exit 1; }
  echo "gap detection: $got missing messages detected = injected: OK"
else
  echo "multicast not available here: skipped"
fi

# 5. pcap output is readable and contains every message (checked with the Python decoder).
"$BUILD/replayer" "$TMP/m.itch" --pcap "$TMP/m.pcap" >/dev/null
python3 - "$TMP/m.pcap" "$TMP/m.itch" <<'PY'
import struct, sys
pcap, itch = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
assert struct.unpack_from('<I', pcap, 0)[0] == 0xa1b2c3d4
off, msgs = 24, []
while off < len(pcap):
    _, _, incl, _ = struct.unpack_from('<IIII', pcap, off); off += 16
    frame = pcap[off:off + incl]; off += incl
    p = frame[42:]
    seq, count = struct.unpack_from('>QH', p, 10)
    if count == 0xFFFF: break
    o = 20
    for _ in range(count):
        (ln,) = struct.unpack_from('>H', p, o); msgs.append(p[o + 2:o + 2 + ln]); o += 2 + ln
o, i = 0, 0
while o < len(itch):
    (ln,) = struct.unpack_from('>H', itch, o)
    assert itch[o + 2:o + 2 + ln] == msgs[i], f"message {i} differs"
    o += 2 + ln; i += 1
assert i == len(msgs)
print(f"pcap: {len(msgs)} messages identical to the ITCH file: OK")
PY
