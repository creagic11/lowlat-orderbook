#!/usr/bin/env python3
"""Reference order book: slow, obviously correct, used as the golden model.

Replays an ITCH 5.0 file and, after every book-changing message (A F E C X D U),
recomputes the best bid and offer of that stock from scratch. Whenever the BBO of
a stock changes it writes one 40-byte little-endian record:

    uint64 seq       index of the message in the file (0-based, all messages)
    uint64 bid_qty   total shares at the best bid (0 if no bids)
    uint64 ask_qty   total shares at the best ask (0 if no asks)
    uint32 bid_px    best bid price, 1e-4 USD (0 if no bids)
    uint32 ask_px    best ask price (0 if no asks)
    uint16 locate    stock locate code
    6 bytes padding

The C++ book must produce exactly the same stream (tests/book_diff_test.cpp and
the `book_diff` tool).

Usage: ref_book.py IN.itch OUT.bbo [--locate N]
"""
from __future__ import annotations

import argparse
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import itchlib as itch  # noqa: E402

REC = struct.Struct("<QQQIIH6x")
assert REC.size == 40


def run(buf: bytes, only_locate: int | None):
    orders = {}  # ref -> [locate, side, price, shares]
    levels = collections.defaultdict(lambda: {"B": collections.Counter(), "S": collections.Counter()})
    last_bbo = {}
    out = bytearray()
    stats = collections.Counter()

    def bbo(loc):
        lv = levels[loc]
        b = max(lv["B"]) if lv["B"] else 0
        a = min(lv["S"]) if lv["S"] else 0
        return (lv["B"][b] if b else 0, lv["S"][a] if a else 0, b, a)

    def add(ref, loc, side, px, sh):
        if ref in orders:  # reused live reference: drop the stale order first
            stats["duplicate_ref"] += 1
            take(ref, orders[ref][3])
        orders[ref] = [loc, side, px, sh]
        levels[loc][side][px] += sh

    def take(ref, sh):
        o = orders.get(ref)
        if o is None:
            stats["unknown_ref"] += 1
            return None
        loc, side, px, rem = o
        sh = min(sh, rem)
        o[3] -= sh
        lv = levels[loc][side]
        lv[px] -= sh
        if lv[px] == 0:
            del lv[px]
        if o[3] == 0:
            del orders[ref]
        return loc

    for seq, m in itch.iter_messages(buf):
        d = itch.decode(m)
        t = d[0]
        if t not in "AEXDU":
            continue
        if t == "A":
            _, loc, ref, side, sh, px = d
            add(ref, loc, side, px, sh)
        elif t in "EX":
            _, loc, ref, sh = d
            loc = take(ref, sh)
        elif t == "D":
            _, loc, ref = d
            o = orders.get(ref)
            loc = take(ref, o[3]) if o else take(ref, 0)
        else:  # U: delete original, add new with same side and stock
            _, loc, ref, new_ref, sh, px = d
            o = orders.get(ref)
            if o is None:
                stats["unknown_ref"] += 1
                continue
            loc, side = o[0], o[1]
            take(ref, o[3])
            add(new_ref, loc, side, px, sh)
        if loc is None or (only_locate is not None and loc != only_locate):
            continue
        cur = bbo(loc)
        if last_bbo.get(loc) != cur:
            last_bbo[loc] = cur
            out += REC.pack(seq, cur[0], cur[1], cur[2], cur[3], loc)
            stats["bbo_updates"] += 1
    stats["live_orders"] = len(orders)
    return bytes(out), stats


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--locate", type=int, default=None, help="only report this stock")
    a = ap.parse_args()
    out, stats = run(Path(a.input).read_bytes(), a.locate)
    Path(a.output).write_bytes(out)
    print(" ".join(f"{k}={v}" for k, v in sorted(stats.items())))


if __name__ == "__main__":
    main()
