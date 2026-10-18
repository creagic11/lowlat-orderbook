# Design notes

Decisions, the reasoning behind them, and what was measured. Numbers marked _indicative_
come from a 2-vCPU cloud VM (clang 18, `-O3`, no `-march=native`). They show direction only;
the numbers that count are measured on the reference laptop and live in `results.md`.

## Feed book: what has to be fast

A market-data book is driven by the exchange. Executions, cancels and deletes name an order
by reference number and carry no price or side, so every one of them starts with
**reference → order** lookup. The book needs to aggregate quantity per price level and to
know the best level of each side. It does **not** need a FIFO per level: the feed already
says which order traded. (The matching engine, Phase 3, does need FIFOs.)

## Order table: open addressing, values inline

`OrderMap<OrderRec>` is a linear-probing table storing `{price, qty, locate, side}` inside
the slot (24 bytes). One lookup is usually one cache line. Deletion uses backward shifting,
so a full trading day of adds and deletes never accumulates tombstones.

**Hash choice.** The default `SequentialRefHash` uses the low bits of the reference
number. Nasdaq hands out references in increasing order, so the live set is mostly a
sliding window of recent references. Low bits map that window to neighbouring slots with
almost no collisions, and the orders most likely to be touched next share cache lines.
`FibonacciHash` (multiplicative) scatters keys and is kept for arbitrary keys.

Indicative, 3M-message synthetic day (200 stocks, 334K resting orders at end):

| Order table | Table sized for | Throughput |
| --- | --- | --- |
| Fibonacci hash | 4M orders (192 MB) | 6.9 M msgs/s |
| Fibonacci hash | 512K orders | 10.4 M msgs/s |
| Low-bits hash | 4M orders | 11.4 M msgs/s |
| Low-bits hash | 512K orders | 11.8 M msgs/s |
| `std::unordered_map` + `std::map` (baseline) | 4M orders | 7.1 M msgs/s |

With a scattering hash, an oversized table turns every lookup into a cache and TLB miss.
The low-bits hash is insensitive to table size because accesses stay local.

## Price levels: sorted vector, best at the back

Each side of each stock is a `std::vector<Level>` sorted worst → best, so the best price is
`back()` and reading the BBO is O(1). Finding a level scans back from the best price (most
feed activity is at or near the top) and falls back to binary search after 8 levels.
Adding or removing a level near the top moves only the few levels above it.

The alternative in the original plan was a flat price-indexed array with a bitmap of
non-empty levels. That needs a fixed price window per stock: memory scales with
`stocks × window` (Nasdaq has ~8,000 symbols), and far-away outlier orders, which real feeds
contain, fall outside the window and need an overflow path. The sorted vector has neither
problem. The bitmap ladder is the right choice in hardware, where a fixed window and a
priority encoder are cheap, so the FPGA parser uses it; Phase 6 benchmarks both in software.

## Verification

`tools/ref_book.py` recomputes each BBO from scratch with dictionaries. Both C++ books must
reproduce its BBO-change stream byte for byte: on the committed 40K-message day in unit
tests, on a fresh 300K-message day in CI, and on a full Nasdaq BX day on the laptop.
