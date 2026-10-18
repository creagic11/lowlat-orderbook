#!/usr/bin/env bash
# Download one Nasdaq BX ITCH 5.0 sample day (391 MB gzipped), verify it, unzip it.
# Run inside WSL2/Linux. Data goes to ~/data (keep it out of /mnt/c: slow) and never into git.
#
#   scripts/fetch_sample.sh [DAY]      default DAY=20191230
set -euo pipefail
DAY="${1:-20191230}"
NAME="${DAY}.BX_ITCH_50"
BASE="https://emi.nasdaq.com/ITCH/Nasdaq%20BX%20ITCH"
DEST="${HOME}/data"
mkdir -p "$DEST" && cd "$DEST"
[ -f "$NAME.gz" ] || curl -fL --retry 3 -o "$NAME.gz" "$BASE/$NAME.gz"
curl -fsSL -o "$NAME.gz.md5sum" "$BASE/$NAME.gz.md5sum" || true
if [ -s "$NAME.gz.md5sum" ]; then
  want=$(awk '{print tolower($1)}' "$NAME.gz.md5sum" | tr -d '\r')
  have=$(md5sum "$NAME.gz" | awk '{print $1}')
  [ "$want" = "$have" ] && echo "md5 OK" || { echo "md5 MISMATCH: $have vs $want"; exit 1; }
fi
[ -f "$NAME" ] || gunzip -k "$NAME.gz"
ls -l "$DEST/$NAME"
echo "First bytes (expect 00 0c 53 = length 12, 'S' system event):"
head -c 16 "$NAME" | od -An -tx1
