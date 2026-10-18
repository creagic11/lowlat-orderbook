# lowlat-orderbook

A low-latency limit order book and Nasdaq TotalView-ITCH 5.0 feed handler in C++20.

> Work in progress. Built phase by phase; each phase lands with tests and its measured results in [`docs/results.md`](docs/results.md).

## Status

| Phase | Scope | Status |
| --- | --- | --- |
| 0 | Repo, CMake, CI (gcc + clang, ASan/UBSan) | done |
| 1 | ITCH 5.0 decoder, synthetic generator, reference model | |
| 2 | Order book (baseline + optimised), verified on a full trading day | |
| 3 | Price-time matching engine | |
| 4 | Lock-free SPSC pipeline on pinned cores | |
| 5 | UDP multicast MoldUDP64 feed handler | |
| 6 | Latency measurement and optimisation | |
| 7 | Write-up | |

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer build: add `-DLOB_SANITIZE=ON` (ASan + UBSan) or `-DLOB_TSAN=ON`.
Benchmark build: add `-DLOB_NATIVE=ON` and run on pinned cores (see `docs/results.md`).
