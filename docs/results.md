# Results

Every number here comes from a command in this repo that anyone can rerun. Each row records
the machine, compiler, flags and date. Numbers from a cloud VM or CI are never used as results.

## Reference machine

| | |
| --- | --- |
| CPU | Intel Core i5-1340P (4 P-cores / 8 E-cores, 16 threads) |
| OS | Windows 11 Home 25H2, WSL2 Ubuntu 24.04 |
| Compiler | g++ 13.3, `-O3 -march=native` (`-DLOB_NATIVE=ON`) |
| Power | plugged in, Windows power mode "Best performance" |

## Phase 1: ITCH 5.0 decoding

**Correctness (synthetic, in CI).** `tests/data/synth_small.itch` is 40,009 messages from
`tools/gen_itch.py --seed 7`. The decoder's counts by type match the generator's ground truth
exactly, with 0 unknown and 0 malformed messages (`ItchFile.SyntheticDayMatchesGroundTruth`).

**Real data.** Nasdaq BX, 30 Dec 2019 (`scripts/fetch_sample.sh 20191230`):

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=ON && cmake --build build
./build/itch_stats ~/data/20191230.BX_ITCH_50 --passes 5
```

| Date | Messages | Unknown / malformed | Decode throughput | Machine |
| --- | --- | --- | --- | --- |
| _pending_ | | | M msgs/s, GB/s | reference laptop |

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

| Date | Book | Messages | BBO stream vs reference | Throughput (best of 5) | ns/msg |
| --- | --- | --- | --- | --- | --- |
| _pending_ | fast | | | M msgs/s | |
| _pending_ | baseline | | | M msgs/s | |

Design choices and indicative cloud-VM measurements: [`design.md`](design.md).

## Phase 3: matching engine

**Correctness.** 40,000 random operations match a naive reference matcher on every trade and
on the book after every operation (unit test `Matching.RandomisedAgainstNaiveMatcher`).

**Throughput.** On the reference laptop: `taskset -c 2 ./build/lob_bench --benchmark_repetitions=5`

| Date | Benchmark | Median | Ops/s |
| --- | --- | --- | --- |
| _pending_ | `BM_MatchingEngineMixedFlow` (60% passive, 10% aggressive, 5% market, 25% cancel) | ns/op | |
