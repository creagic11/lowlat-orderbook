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

`OrderMap<OrderRec>` is a linear-probing table with Robin Hood ordering, storing
`{price, qty, locate, side}` inside the slot (24 bytes). One lookup is usually one cache
line. Deletion uses backward shifting, so a full trading day of adds and deletes never
accumulates tombstones.

**Why Robin Hood: a bug the micro-benchmark caught.** The first version used plain linear
probing. With sequential references and the low-bits hash, the live orders occupy one long
run of consecutive slots. Plain backward-shift deletion has to scan to the end of that run
before it may stop, so every delete cost O(live orders): 2,564 ns per add+delete pair
against 80 ns for `std::unordered_map`. Robin Hood ordering keeps entries sorted by probe
distance, so a delete stops at the first following entry that sits in its home slot, which
for sequential references is the very next one. After the change: 110 ns, then 102 ns once
`add` stopped doing a separate duplicate check before inserting.

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

## Micro-benchmarks vs whole-day replay

Indicative, Google Benchmark, one stock, about 2,000 resting orders, add + delete:

| Book | Prices uniform over 32 levels | Prices near the top (geometric) |
| --- | --- | --- |
| FastBook | 102 ns | 56 ns |
| BaselineBook | 75 ns | 54 ns |

With one small stock everything fits in L1 for both books, and `std::map` over 32 levels is
a five-step in-cache walk, so the books are close (uniform depth is `std::map`'s best case).
The whole-day replay (200 stocks, 334K resting orders, 3M messages) is where memory layout
matters: FastBook 11.3 M msgs/s against 7.7 M msgs/s (1.46×), identical BBO streams. That
gap comes from fewer cache misses, so the laptop measurement on a real Nasdaq day is the
number that counts.

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

## Matching engine

Unlike the feed book, the engine decides who trades, so each level keeps a FIFO of resting
orders: pooled nodes (free list, 32-bit index links), per-level head/tail, and an id table
using the scattering hash because client order ids can be arbitrary. Same sorted-vector
levels with the best at the back. Matching walks the best opposite level's FIFO, removes
filled orders in O(1), and pops the level when it empties.

Verification: 40,000 random operations (limit GTC/IOC, market, cancel, modify) in a narrow
price band, compared on every trade and on the book after every operation against a naive
matcher that scans a flat list (12,955 trades, 5,759 modifies, 8,758 cancels; the book is
checked never to be crossed).

## Pipeline ring (Phase 4)

`SpscRing<T, N>` (`spsc.hpp`): a power-of-two array of slots, the producer's `tail` and the
consumer's `head` each alone on a 64-byte cache line, published with release and read with
acquire. Each side keeps a private copy of the other side's index and re-reads the shared one only
when the ring looks full (producer) or empty (consumer), so in steady state a push or a pop touches
no line the other core is writing. `pop_batch` copies up to 64 slots and publishes the new head
once. The template switches `CacheIndices` and `Pad` exist only to measure what each choice is
worth; the results (laptop, full day) are in `results.md`: without cached indices the median
latency rises 62%, without padding 32%, and popping one at a time costs 40% of throughput.

A test caught one subtlety: with cached indices a batch pop can see fewer items than are really
there (its cached tail is stale), which is correct but under-fills batches; `pop_batch` refreshes
the cached tail once when the batch would come up short.

## Verification

`tools/ref_book.py` recomputes each BBO from scratch with dictionaries. Both C++ books must
reproduce its BBO-change stream byte for byte: on the committed 40K-message day in unit
tests, on a fresh 300K-message day in CI, and on a full Nasdaq BX day on the laptop.
