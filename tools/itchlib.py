"""Minimal Nasdaq TotalView-ITCH 5.0 encoder/decoder shared by the Python tools.

Layouts follow the Nasdaq ITCH 5.0 specification. All integers big-endian,
prices are uint32 with 4 implied decimals, timestamps are 6-byte ns since midnight.
Only what the tools need is implemented: S, R, A, F, E, C, X, D, U, P.
"""
from __future__ import annotations

import struct
from typing import Iterator, Tuple

MESSAGE_SIZE = {
    "S": 12, "R": 39, "H": 25, "Y": 20, "L": 26, "V": 35, "W": 12, "K": 28,
    "J": 35, "h": 21, "A": 36, "F": 40, "E": 31, "C": 36, "X": 23, "D": 19,
    "U": 35, "P": 44, "Q": 40, "B": 19, "I": 50, "N": 20, "O": 48,
}
BOOK_TYPES = set("AFECXDU")


def _hdr(t: str, locate: int, tracking: int, ts: int) -> bytes:
    return t.encode() + struct.pack(">HH", locate, tracking) + ts.to_bytes(6, "big")


def _stock(s: str) -> bytes:
    return s.encode().ljust(8, b" ")[:8]


def system_event(ts: int, code: str) -> bytes:
    return _hdr("S", 0, 0, ts) + code.encode()


def stock_directory(ts: int, locate: int, stock: str, round_lot: int = 100) -> bytes:
    # stock(8) market cat(1) fin status(1) round lot(4) round lots only(1) issue class(1)
    # issue subtype(2) authenticity(1) short sale thr(1) IPO(1) LULD tier(1) ETP(1)
    # ETP leverage(4) inverse(1)
    body = (_stock(stock) + b"Q" + b"N" + struct.pack(">I", round_lot) + b"N" + b"C" + b"Z "
            + b"P" + b"N" + b"N" + b"1" + b"N" + struct.pack(">I", 0) + b"N")
    return _hdr("R", locate, 0, ts) + body


def add(ts, locate, ref, side, shares, stock, price, mpid=None) -> bytes:
    t = "F" if mpid else "A"
    m = _hdr(t, locate, 0, ts) + struct.pack(">Q", ref) + side.encode() + struct.pack(
        ">I", shares) + _stock(stock) + struct.pack(">I", price)
    if mpid:
        m += mpid.encode().ljust(4, b" ")[:4]
    return m


def executed(ts, locate, ref, shares, match) -> bytes:
    return _hdr("E", locate, 0, ts) + struct.pack(">QIQ", ref, shares, match)


def executed_price(ts, locate, ref, shares, match, price, printable="Y") -> bytes:
    return (_hdr("C", locate, 0, ts) + struct.pack(">QIQ", ref, shares, match)
            + printable.encode() + struct.pack(">I", price))


def cancel(ts, locate, ref, shares) -> bytes:
    return _hdr("X", locate, 0, ts) + struct.pack(">QI", ref, shares)


def delete(ts, locate, ref) -> bytes:
    return _hdr("D", locate, 0, ts) + struct.pack(">Q", ref)


def replace(ts, locate, ref, new_ref, shares, price) -> bytes:
    return _hdr("U", locate, 0, ts) + struct.pack(">QQII", ref, new_ref, shares, price)


def trade(ts, locate, ref, side, shares, stock, price, match) -> bytes:
    return (_hdr("P", locate, 0, ts) + struct.pack(">Q", ref) + side.encode()
            + struct.pack(">I", shares) + _stock(stock) + struct.pack(">IQ", price, match))


def frame(msg: bytes) -> bytes:
    """Nasdaq sample-file framing: 2-byte big-endian length, then the message."""
    return struct.pack(">H", len(msg)) + msg


def iter_messages(buf: bytes) -> Iterator[Tuple[int, memoryview]]:
    """Yield (index, message) for a length-prefixed ITCH file buffer."""
    mv = memoryview(buf)
    off, i, n = 0, 0, len(buf)
    while off + 2 <= n:
        ln = (buf[off] << 8) | buf[off + 1]
        if off + 2 + ln > n:
            raise ValueError(f"truncated message at offset {off}")
        yield i, mv[off + 2: off + 2 + ln]
        off += 2 + ln
        i += 1
    if off != n:
        raise ValueError(f"trailing {n - off} bytes")


def decode(m: memoryview):
    """Decode a book message into a tuple; returns (type, fields...) or (type,) for others."""
    t = chr(m[0])
    locate = (m[1] << 8) | m[2]
    if t in "AF":
        ref, = struct.unpack_from(">Q", m, 11)
        side = chr(m[19])
        shares, = struct.unpack_from(">I", m, 20)
        price, = struct.unpack_from(">I", m, 32)
        return ("A", locate, ref, side, shares, price)
    if t == "E" or t == "C":
        ref, shares = struct.unpack_from(">QI", m, 11)
        return ("E", locate, ref, shares)
    if t == "X":
        ref, shares = struct.unpack_from(">QI", m, 11)
        return ("X", locate, ref, shares)
    if t == "D":
        ref, = struct.unpack_from(">Q", m, 11)
        return ("D", locate, ref)
    if t == "U":
        ref, new_ref, shares, price = struct.unpack_from(">QQII", m, 11)
        return ("U", locate, ref, new_ref, shares, price)
    return (t,)
