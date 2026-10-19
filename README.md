# lowlat-orderbook

A low-latency limit order book and Nasdaq TotalView-ITCH 5.0 feed handler in C++20.

> Work in progress. Built phase by phase; each phase lands with tests and its measured results in [`docs/results.md`](docs/results.md).

## Status

| Phase | Scope | Status |
| --- | --- | --- |
| 0 | Repo, CMake, CI (gcc + clang, ASan/UBSan) | done |
| 1 | ITCH 5.0 decoder, synthetic generator, reference model | done: 29.2M real messages, 0 errors, 139.7 M msgs/s |
| 2 | Order book (baseline + optimised), verified against a reference model | done: identical on a full Nasdaq BX day; 15.7 vs 9.8 M msgs/s (1.60×) |
| 3 | Price-time matching engine | done |
| 4 | Lock-free SPSC pipeline on pinned cores | done: full day identical; 11.1-12.5 M msgs/s on 2 threads vs 6.8 on 1 (1.64-1.83x) |
| 5 | UDP multicast MoldUDP64 feed handler | done: full real day over multicast, 0 gaps at 1.5 M msgs/s, identical BBO stream |
| 6 | Latency measurement and optimisation | done: decode-to-BBO p50 34 ns (1 thread), 159 ns (2 threads); ring ablations: cached indices -38% p50, padding -24% p50, batching +68% throughput |
| 7 | Write-up | done: [docs/writeup.md](docs/writeup.md) |

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer build: add `-DLOB_SANITIZE=ON` (ASan + UBSan) or `-DLOB_TSAN=ON`.
Benchmark build: add `-DLOB_NATIVE=ON` and run on pinned cores (see `docs/results.md`).

## Real market data

Nasdaq publishes free full-day ITCH 5.0 sample files. `scripts/fetch_sample.sh` downloads one
BX day (391 MB gzipped) into `~/data`, checks its md5 and unzips it. The data never goes into git.

## Tools

| Tool | What it does |
| --- | --- |
| `itch_stats FILE` | Decodes a file, counts messages by type, reports decode throughput |
| `tools/gen_itch.py` | Synthetic ITCH day with known ground truth (uncrossed book, valid references) |
| `tools/ref_book.py` | Slow, obviously correct reference book; writes the BBO stream the C++ book must match |
| `replay FILE --book fast\|baseline` | Replays a file through a book; `--compare REF.bbo` checks it against the reference |
| `replayer FILE --send \| --pcap OUT` | Packs the file into MoldUDP64 packets: UDP multicast (paced with `--rate`, `--drop-every N` to inject loss) and/or a pcap of Ethernet/IPv4/UDP frames |
| `feed` | Joins the multicast group, receives with `recvmmsg`, detects sequence gaps and duplicates, builds the book; `--compare REF.bbo` |

## Design

- **Order table:** open addressing with inline values and backward-shift deletion, indexed by the low bits of the (increasing) order reference.
- **Price levels:** per stock and side, a vector sorted worst → best; the BBO is `back()`.
- **Baseline:** `std::unordered_map` + `std::map`, same interface, for comparison.
- **Matching engine:** price-time priority; limit (GTC/IOC), market, cancel, modify (a size decrease keeps priority, a price change loses it); pooled orders in per-level FIFOs.

Reasoning and measurements: [`docs/design.md`](docs/design.md).
