# A Nasdaq ITCH order book in C++20: from file to multicast to two cores

This is the write-up for `lowlat-orderbook`: a market-data stack that decodes Nasdaq TotalView-ITCH
5.0, maintains limit order books for every stock on the feed, receives the feed over UDP multicast
in MoldUDP64 packets, and splits the work across two cores with a lock-free ring. Everything is
checked against an independent reference on a full real trading day (Nasdaq BX, 30 Dec 2019,
29,156,757 messages), and every number comes from one laptop (Intel i5-1340P, a 2-vCPU Linux VM,
g++ 11.4 `-O3 -march=native`). Details and raw tables: [results.md](results.md),
design reasoning: [design.md](design.md).

## 1. What it does

```
pcap / file / UDP multicast  ->  MoldUDP64 (sequence, gaps)  ->  ITCH decode  ->  [SPSC ring]  ->  book  ->  BBO changes
```

- **Decoder** (`itch.hpp`): all 23 ITCH 5.0 message types, big-endian fields read with byte
  swaps, book events reduced to one 40-byte `Event`.
- **Book** (`book.hpp`): per-stock bid and ask ladders plus one global order table, reporting a
  best-bid-and-offer record whenever a stock's top of book changes.
- **Matching engine** (`matching_engine.hpp`): a price-time priority engine (limit, market,
  cancel) with FIFO queues per level, for the other side of the problem.
- **Feed handler** (`feed`): MoldUDP64 over UDP multicast with `recvmmsg`, sequence tracking and
  gap detection, plus a replayer that turns an ITCH file into the same packets.
- **Pipeline** (`pipeline`): feed thread and book thread on separate cores joined by a lock-free
  single-producer single-consumer ring, with per-event latency measurement.

## 2. Results

| | |
| --- | --- |
| Decode | 29.2M real messages, 0 errors, **139.7 M msgs/s** (4.14 GB/s) |
| Book, one thread | **15.7 M msgs/s** (63.7 ns/msg) on the full day, 1.60x a `std::map` baseline; BBO stream identical to the Python reference (22,397,555 records) |
| Book, two threads (pipeline) | **11.1-12.5 M msgs/s** with a latency stamp and histogram update per event, vs 6.8 M on one thread doing the same (1.64-1.83x); BBO stream identical |
| Decode-to-BBO latency | paced at 1 M events/s: **p50 34 ns** on one thread, **159 ns** on two (p90 182 / 459 ns) |
| Multicast feed | full day over loopback multicast, **0 gaps at 1.5 M msgs/s**, BBO stream identical; above that the VM's socket buffer cap loses packets, and every loss is detected by sequence number |
| Matching engine | 40,000 random operations identical to a naive matcher on every trade and every book state (throughput benchmark not yet run on the laptop) |

## 3. Decisions that mattered

**Order table: open addressing with Robin Hood ordering, keyed by the low bits.** Every execute,
cancel and delete names an order by reference and carries nothing else, so the reference-to-order
lookup is the hot path. Nasdaq hands out references in increasing order, so hashing by the low
bits places the live orders, a sliding window of recent references, in neighbouring slots. The
first version used plain linear probing and a micro-benchmark caught a disaster: those live orders
form one long run, plain backward-shift deletion scans to its end, and every delete cost
O(live orders), 2,564 ns per add and delete. Robin Hood ordering lets a delete stop at the next
entry in its home slot, which for sequential references is the very next one: 102 ns.

**Price levels: a sorted vector per side with the best price at the back.** Almost all activity
is near the top of book, so inserting or removing a level is a short move at the end of a vector,
and the best price is `back()`. It beat a `std::map` ladder by 1.6x on the real day.

**The ring: three choices, each measured.** The pipeline ring is a power-of-two array with the
producer's index and the consumer's index on separate cache lines; each side keeps a private copy
of the other side's index and re-reads the shared one only when the ring looks full or empty; the
consumer pops in batches and publishes its index once per batch. Turning each choice off on the
same day of data:

| Ring variant | p50 latency (paced) | Throughput (unpaced) |
| --- | --- | --- |
| all three | **155 ns** | **12.45 M msgs/s** |
| no cached indices | 251 ns (+62%) | 9.67 M msgs/s (-22%) |
| no padding (false sharing) | 204 ns (+32%) | 12.31 M msgs/s |
| no batching | 167 ns | 7.41 M msgs/s (-40%) |

Cached indices matter most for latency (each pop otherwise pulls the producer's cache line);
batching matters most for throughput (one index publish per 64 events instead of per event).

**Measuring latency honestly.** Timestamps come from the TSC (an `rdtsc` costs about 20 cycles;
the counter is invariant on this CPU, so stamps taken on one core compare with reads on another),
calibrated against `steady_clock` at start-up. Latencies go into a fixed-memory histogram, exact
to the nanosecond below 16 us. Unpaced, the feed outruns the book and the ring fills, so "latency"
becomes time spent waiting in a full queue (0.4 ms at the median): the latency figures are
therefore taken with the feed paced at 1 M events/s, where the ring never queues.

## 4. What the numbers do not say

- **The two-thread tail is the VM.** At p99 the two-thread pipeline shows 183 us. The VM has
  exactly two vCPUs and both threads busy-spin, so any kernel or hypervisor interruption preempts
  a stage and events queue behind it; the one-thread run, which leaves a vCPU free, has a 454 ns
  p99. Isolated cores (`isolcpus`, `nohz_full`, IRQ affinity) on bare metal are the fix, and I could
  not test them here.
- **Two threads are not faster per event.** The handoff costs about 125 ns at the median. The
  point of the split is that the feed thread never waits on the book: it keeps draining the socket
  while the book catches up after a burst, and throughput rises because decoding and booking
  overlap.
- **Multicast was loopback on one machine**, limited by the VM's socket buffer cap; a real NIC,
  kernel-bypass (`AF_XDP`, DPDK) and hardware timestamps would change both the ceiling and the
  latency.
- All numbers are from one laptop VM whose vCPUs Windows schedules onto P-cores or E-cores; runs
  were repeated and ranges are reported where they varied.

## 5. Verification

- GoogleTest suites for the decoder, order table, books, matching engine, MoldUDP64, SPSC ring and
  latency histogram, built with GCC and Clang and run under AddressSanitizer and
  UndefinedBehaviorSanitizer in CI; the ring's two-thread tests also run clean under
  ThreadSanitizer.
- Golden streams: a Python reference book (`tools/ref_book.py`) produced the BBO stream for the
  full day; the C++ books, the multicast feed handler and the two-thread pipeline all reproduce it
  record for record. CTest runs the pipeline in each ring variant against a reference file.
- A randomised test drives the matching engine and a naive matcher with the same 40,000 operations.

## 6. Next

- Matching-engine throughput benchmark on the laptop.
- Kernel-bypass receive (`AF_XDP`) and hardware timestamps for wire-to-BBO latency.
- The same pipeline on isolated cores of a bare-metal Linux machine, to separate the ring's tail
  from the scheduler's.
- The FPGA counterpart, [fpga-itch-parser](https://github.com/creagic11/fpga-itch-parser), does the
  framing, decoding and top of book for one stock in hardware at 156.25 MHz, verified against this
  book on the same data.
