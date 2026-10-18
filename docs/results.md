# Results

Every number here comes from a command in this repo that anyone can rerun. Each row records
the machine, compiler, flags and date. Numbers from a cloud VM or CI are never used as results.

## Reference machine

| | |
| --- | --- |
| CPU | Intel Core i5-1340P (4 P-cores / 8 E-cores, 16 threads) |
| Environment | Linux VM on the laptop (Ubuntu 22.04, kernel 6.8, 2 vCPUs, 3 GB RAM) |
| Compiler | g++ 11.4, `-std=c++20 -O3 -march=native -DNDEBUG` |
| Pinning | `taskset -c 1` |

The VM's vCPUs are scheduled by Windows, so a run can land on a P-core or an E-core; numbers are
the best of 5 passes and were repeated to check they are stable. Data: Nasdaq BX ITCH 5.0,
30 Dec 2019 (`20191230.BX_ITCH_50`, 390,561,039 bytes gzipped).

## Phase 1: ITCH 5.0 decoding

**Correctness (synthetic, in CI).** `tests/data/synth_small.itch` is 40,009 messages from
`tools/gen_itch.py --seed 7`. The decoder's counts by type match the generator's ground truth
exactly, with 0 unknown and 0 malformed messages (`ItchFile.SyntheticDayMatchesGroundTruth`).

**Real data.** Nasdaq BX, 30 Dec 2019 (`scripts/fetch_sample.sh 20191230`):

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=ON && cmake --build build
./build/itch_stats ~/data/20191230.BX_ITCH_50 --passes 5
```

| Date | Messages | Unknown / malformed | Decode throughput (best of 5) | Machine |
| --- | --- | --- | --- | --- |
| 2026-10-19 | 29,156,757 (862.9 MB) | 0 / 0 | 139.7 M msgs/s, 4.14 GB/s | reference laptop, Linux sandbox (see below) |

By type: A 12,210,139 · D 11,821,540 · U 1,741,672 · E 578,839 · X 348,198 · F 45,058 · C 2,686 ·
P 134,385 · N 2,241,182 · R 8,906 · Y 9,013 · H 8,961 · L 6,171 · S 6 · V 1. The file starts
`00 0c 53`: a 2-byte length (12) then an `S` system event, confirming the framing.

## Phase 2: order book

**Correctness.** Both books reproduce `tools/ref_book.py`'s BBO-change stream byte for byte:
- 40,009-message synthetic day (unit test, 9,695 BBO changes)
- fresh 300,000-message day, 50 stocks (CI self-test)
- 3,000,206-message day, 200 stocks (725,827 BBO changes; run during development)

**Real data and throughput.** On the reference laptop:

```bash
python3 tools/ref_book.py ~/data/20191230.BX_ITCH_50 ~/data/bx.bbo        # slow: run once
./build/replay ~/data/20191230.BX_ITCH_50 --book fast     --compare ~/data/bx.bbo
./build/replay ~/data/20191230.BX_ITCH_50 --book baseline --compare ~/data/bx.bbo
taskset -c 2 ./build/replay ~/data/20191230.BX_ITCH_50 --book fast     --passes 5
taskset -c 2 ./build/replay ~/data/20191230.BX_ITCH_50 --book baseline --passes 5
```

| Date | Book | Messages | BBO stream vs reference | Throughput (best of 5, pinned) | ns/msg |
| --- | --- | --- | --- | --- | --- |
| 2026-10-19 | fast | 29,156,757 | 22,397,555 records identical | 15.70 M msgs/s | 63.7 |
| 2026-10-19 | baseline | 29,156,757 | 22,397,555 records identical | 9.80 M msgs/s | 102.0 |

**FastBook is 1.60× the baseline on a full real trading day** (two rounds of 5 passes each:
15.57/15.70 vs 9.79/9.80 M msgs/s). Both books end the day with 0 live orders and see 0 unknown
or duplicate references, as does the reference model (`ref_book.py`: 22,397,555 BBO changes).

Design choices and indicative cloud-VM measurements: [`design.md`](design.md).

## Phase 3: matching engine

**Correctness.** 40,000 random operations match a naive reference matcher on every trade and
on the book after every operation (unit test `Matching.RandomisedAgainstNaiveMatcher`).

**Throughput.** On the reference laptop: `taskset -c 2 ./build/lob_bench --benchmark_repetitions=5`

| Date | Benchmark | Median | Ops/s |
| --- | --- | --- | --- |
| _pending_ | `BM_MatchingEngineMixedFlow` (60% passive, 10% aggressive, 5% market, 25% cancel) | ns/op | |

## Phase 5: UDP multicast feed handler

The replayer packs the BX day into MoldUDP64 packets (≤ 1,400 bytes, 632,413 packets) and
multicasts them on loopback; the feed handler (`recvmmsg`, gap detection, FastBook) runs on
the other vCPU. Sender `taskset -c 0`, receiver `taskset -c 1`.

| Date | Send rate | Gaps (messages missing) | BBO stream vs reference | Notes |
| --- | --- | --- | --- | --- |
| 2026-10-19 | 1.50 M msgs/s (paced) | 0 (0) | 22,397,555 records identical | full day in 19.4 s; 48 packets per `recvmmsg` |
| 2026-10-19 | 3.00 M msgs/s (paced) | 2 (9,349) | — | bursts overflow the receive buffer |
| 2026-10-19 | 5.00 M msgs/s (paced) | 6 (18,157) | — | |
| 2026-10-19 | 7.62 M msgs/s (flat out) | 197 (502,311) | — | |

The receive buffer is the limit: the VM's `net.core.rmem_max` is 212,992, so the socket gets
416 KB (about 300 packets, a few milliseconds of traffic), and the VM's vCPUs can be paused
by the host for longer than that. Raising `rmem_max` needs root, which the VM does not allow.
Every loss was detected and counted by sequence number; none went unnoticed. For scale, the
BX day averages about 1,250 messages per second over its 6.5 trading hours.

Gap detection itself is tested exactly in CI: with every 25th packet dropped by the replayer,
the number of missing messages the feed reports equals the number dropped.
