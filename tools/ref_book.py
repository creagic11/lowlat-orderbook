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

The C++ books must produce exactly the same stream (`replay --compare`, and the
unit test BookTest.SyntheticDayMatchesReferenceBboStream).

Usage: ref_book.py IN.itch OUT.bbo [--locate N] [--budget SECONDS --state CKPT]
A full trading day can be processed in several runs: with --budget the tool stops
after about that long, saves its state to CKPT, and the next run resumes from it
and appends to OUT.
"""
from __future__ import annotations

import argparse
import mmap
import pickle
import time
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import itchlib as itch  # noqa: E402

REC = struct.Struct("<QQQIIH6x")
assert REC.size == 40


def new_state():
    return {"orders": {}, "levels": {}, "last_bbo": {}, "stats": collections.Counter(),
            "offset": 0, "seq": 0}


def run(buf, only_locate: int | None, st: dict, budget_s: float | None = None):
    """Process messages from st["offset"] on; stop at the end of buf or when the time
    budget runs out. Returns (BBO records produced, finished?)."""
    orders = st["orders"]          # ref -> [locate, side, price, shares]
    levels = st["levels"]          # locate -> {"B": Counter(price->qty), "S": Counter}
    last_bbo = st["last_bbo"]
    stats = st["stats"]
    out = bytearray()
    deadline = time.monotonic() + budget_s if budget_s else None

    def side_levels(loc, side):
        lv = levels.get(loc)
        if lv is None:
            lv = levels[loc] = {"B": collections.Counter(), "S": collections.Counter()}
        return lv[side]

    def bbo(loc):
        lv = levels.get(loc) or {"B": {}, "S": {}}
        b = max(lv["B"]) if lv["B"] else 0
        a = min(lv["S"]) if lv["S"] else 0
        return (lv["B"][b] if b else 0, lv["S"][a] if a else 0, b, a)

    def take(ref, sh):
        o = orders.get(ref)
        if o is None:
            stats["unknown_ref"] += 1
            return None
        loc, side, px, rem = o
        sh = min(sh, rem)
        o[3] -= sh
        lv = side_levels(loc, side)
        lv[px] -= sh
        if lv[px] == 0:
            del lv[px]
        if o[3] == 0:
            del orders[ref]
        return loc

    def add(ref, loc, side, px, sh):
        if ref in orders:  # reused live reference: drop the stale order first
            stats["duplicate_ref"] += 1
            take(ref, orders[ref][3])
        orders[ref] = [loc, side, px, sh]
        side_levels(loc, side)[px] += sh

    n, off, seq = len(buf), st["offset"], st["seq"]
    finished = True
    while off + 2 <= n:
        if deadline and (seq & 0xFFFF) == 0 and time.monotonic() > deadline:
            finished = False
            break
        ln = (buf[off] << 8) | buf[off + 1]
        m = memoryview(buf)[off + 2: off + 2 + ln]
        off += 2 + ln
        cur_seq, seq = seq, seq + 1
        t = chr(m[0])
        if t not in "AFECXDU":
            continue
        d = itch.decode(m)
        if t in "AF":
            _, loc, ref, side, sh, px = d
            add(ref, loc, side, px, sh)
        elif t in "ECX":
            loc = take(d[2], d[3])
        elif t == "D":
            o = orders.get(d[2])
            loc = take(d[2], o[3] if o else 0)
        else:  # U: delete original, add new with the same side and stock
            _, _, ref, new_ref, sh, px = d
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
            out += REC.pack(cur_seq, cur[0], cur[1], cur[2], cur[3], loc)
            stats["bbo_updates"] += 1
    st["offset"], st["seq"] = off, seq
    stats["live_orders"] = len(orders)
    return bytes(out), finished


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--locate", type=int, default=None, help="only report this stock")
    ap.add_argument("--budget", type=float, default=None,
                    help="stop after about this many seconds and save a checkpoint")
    ap.add_argument("--state", default=None,
                    help="checkpoint file: resume from it if present, save to it when stopping")
    a = ap.parse_args()
    st = new_state()
    resuming = a.state and Path(a.state).exists()
    if resuming:
        with open(a.state, "rb") as f:
            st = pickle.load(f)
    with open(a.input, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as buf:
        out, finished = run(buf, a.locate, st, a.budget)  # mapped, not loaded
    with open(a.output, "ab" if resuming else "wb") as f:
        f.write(out)
    if a.state:
        if finished:
            Path(a.state).unlink(missing_ok=True)
        else:
            with open(a.state, "wb") as f:
                pickle.dump(st, f, protocol=pickle.HIGHEST_PROTOCOL)
    print(("DONE " if finished else f"PAUSED at message {st['seq']} ")
          + " ".join(f"{k}={v}" for k, v in sorted(st["stats"].items())))


if __name__ == "__main__":
    main()
