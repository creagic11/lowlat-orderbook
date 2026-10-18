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
