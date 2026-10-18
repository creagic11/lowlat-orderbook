#!/usr/bin/env python3
"""Synthetic ITCH 5.0 generator with known ground truth.

Writes a Nasdaq-sample-style file (2-byte length prefix per message) that looks
like a small trading day: system events, a stock directory, then adds, executions,
cancels, deletes, replaces and hidden trades around a moving price. The book is
kept uncrossed (buys are always below the best ask, sells above the best bid), as
on a real exchange feed. Every message is valid: executions and cancels never
exceed an order's remaining shares, and every referenced order exists.

Ground truth goes to <out>.counts (one "<type> <count>" per line) so the C++
tests can check the decoder without a JSON parser.

Usage: gen_itch.py --messages 40000 --stocks 3 --seed 7 --out tests/data/synth_small.itch
"""
from __future__ import annotations

import argparse
import collections
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import itchlib as itch  # noqa: E402

TICK = 100  # $0.01 in ITCH price units (4 implied decimals)


class Stock:
    def __init__(self, locate: int, name: str, mid: int):
        self.locate, self.name, self.mid = locate, name, mid
        self.levels = {"B": collections.Counter(), "S": collections.Counter()}

    def best(self, side: str):
        lv = self.levels[side]
        if not lv:
            return None
        return max(lv) if side == "B" else min(lv)

    def pick_price(self, side: str, rng: random.Random) -> int:
        off = rng.choice([1, 1, 2, 2, 3, 4, 5, 6, 8, 10, 15, 25]) * TICK
        if side == "B":
            ask = self.best("S")
            anchor = ask if ask is not None else self.mid + TICK
            return max(TICK, anchor - off)
        bid = self.best("B")
        anchor = bid if bid is not None else self.mid - TICK
        return anchor + off


def generate(n_messages: int, n_stocks: int, seed: int):
    rng = random.Random(seed)
    out: list[bytes] = []
    counts: collections.Counter = collections.Counter()

    def emit(m: bytes):
        out.append(itch.frame(m))
        counts[chr(m[0])] += 1

    ts = 4 * 3600 * 10**9  # 04:00:00, start of system hours
    stocks = [Stock(i + 1, f"TST{i + 1:02d}", rng.randint(20, 400) * 10_000)
              for i in range(n_stocks)]
    emit(itch.system_event(ts, "O"))
    for s in stocks:
        ts += 1000
        emit(itch.stock_directory(ts, s.locate, s.name))
    emit(itch.system_event(ts, "S"))
    ts = 9 * 3600 * 10**9 + 30 * 60 * 10**9
    emit(itch.system_event(ts, "Q"))

    orders: dict[int, list] = {}        # ref -> [stock, side, price, shares]
    live: list[int] = []                # refs, for O(1) random choice
    pos: dict[int, int] = {}            # ref -> index in live
    by_level: dict = collections.defaultdict(collections.OrderedDict)  # (loc,side,px) -> refs FIFO
    next_ref, match = 1000, 1

    def insert(ref, st, side, price, shares):
        orders[ref] = [st, side, price, shares]
        pos[ref] = len(live)
        live.append(ref)
        st.levels[side][price] += shares
        by_level[(st.locate, side, price)][ref] = None

    def remove(ref):
        st, side, price, shares = orders.pop(ref)
        i = pos.pop(ref)
        last = live.pop()
        if last != ref:
            live[i] = last
            pos[last] = i
        st.levels[side][price] -= shares
        if st.levels[side][price] == 0:
            del st.levels[side][price]
        q = by_level[(st.locate, side, price)]
        del q[ref]
        if not q:
            del by_level[(st.locate, side, price)]

    def reduce(ref, by):
        o = orders[ref]
        o[3] -= by
        o[0].levels[o[1]][o[2]] -= by

    mix = [("A", 40), ("F", 4), ("E", 11), ("C", 2), ("X", 8), ("D", 25), ("U", 7), ("P", 3)]
    kinds, weights = zip(*mix)

    book_msgs = 0
    while book_msgs < n_messages:
        ts += rng.randint(0, 50_000)
        kind = rng.choices(kinds, weights)[0]
        if kind not in "AFP" and len(live) < 50:
            kind = "A"
        if kind in "AF":
            st = rng.choice(stocks)
            side = rng.choice("BS")
            price = st.pick_price(side, rng)
            shares = rng.choice([100, 100, 100, 200, 300, 500, 1000]) * rng.randint(1, 3)
            ref = next_ref
            next_ref += rng.randint(1, 4)
            insert(ref, st, side, price, shares)
            emit(itch.add(ts, st.locate, ref, side, shares, st.name, price,
                          mpid=("GSCO" if kind == "F" else None)))
        elif kind in "EC":
            # Aggressive flow takes the oldest order at the best price of one side.
            st = rng.choice(stocks)
            side = rng.choice("BS")
            best = st.best(side)
            if best is None:
                continue
            ref = next(iter(by_level[(st.locate, side, best)]))
            shares = orders[ref][3]
            qty = shares if rng.random() < 0.6 else rng.randint(1, shares)
            if kind == "E":
                emit(itch.executed(ts, st.locate, ref, qty, match))
            else:
                emit(itch.executed_price(ts, st.locate, ref, qty, match,
                                         best + rng.choice([-TICK, 0, TICK])))
            match += 1
            if qty == shares:
                remove(ref)
            else:
                reduce(ref, qty)
        elif kind == "X":
            ref = rng.choice(live)
            st, _, _, shares = orders[ref]
            if shares < 2:
                continue
            qty = rng.randint(1, shares - 1)
            reduce(ref, qty)
            emit(itch.cancel(ts, st.locate, ref, qty))
        elif kind == "D":
            ref = rng.choice(live)
            st = orders[ref][0]
            remove(ref)
            emit(itch.delete(ts, st.locate, ref))
        elif kind == "U":
            ref = rng.choice(live)
            st, side, _, _ = orders[ref]
            remove(ref)
            price = st.pick_price(side, rng)
            shares = rng.choice([100, 200, 300, 500]) * rng.randint(1, 3)
            new_ref = next_ref
            next_ref += rng.randint(1, 4)
            insert(new_ref, st, side, price, shares)
            emit(itch.replace(ts, st.locate, ref, new_ref, shares, price))
        else:  # P: hidden order traded; no book change
            st = rng.choice(stocks)
            px = st.best("S") or st.mid
            emit(itch.trade(ts, st.locate, 0, rng.choice("BS"), rng.randint(1, 5) * 100,
                            st.name, px, match))
            match += 1
        book_msgs += 1

    ts += 10**9
    for code in "MEC":
        emit(itch.system_event(ts, code))
        ts += 1000
    return b"".join(out), counts, len(live)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--messages", type=int, default=40_000, help="book messages to generate")
    ap.add_argument("--stocks", type=int, default=3)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    data, counts, live = generate(a.messages, a.stocks, a.seed)
    Path(a.out).write_bytes(data)
    with open(a.out + ".counts", "w") as f:
        f.write(f"total {sum(counts.values())}\n")
        for t in sorted(counts):
            f.write(f"{t} {counts[t]}\n")
        f.write(f"live_orders {live}\n")
    print(f"wrote {a.out}: {sum(counts.values())} messages, {len(data)} bytes, "
          f"{live} orders resting at end")


if __name__ == "__main__":
    main()
