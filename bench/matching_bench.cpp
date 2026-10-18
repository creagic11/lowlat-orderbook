// Matching engine and feed-book micro-benchmarks (Google Benchmark).
// Run on the reference laptop:  taskset -c 2 ./build/lob_bench --benchmark_repetitions=5

#include <benchmark/benchmark.h>

#include <algorithm>
#include <random>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_baseline.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;

// Steady-state order flow around a fixed mid: ~60% passive limits, 10% aggressive
// limits, 5% market, 25% cancels. The book holds roughly 1,000 resting orders.
static void BM_MatchingEngineMixedFlow(benchmark::State& state) {
    MatchingEngine e(1 << 16);
    std::mt19937 rng(1);
    std::vector<Trade> trades;
    trades.reserve(64);
    std::vector<std::uint64_t> ids;
    std::uint64_t next = 1;
    // Pre-generate the flow so random number generation is not timed.
    struct Op { std::uint8_t kind; Side side; std::uint32_t px, qty; std::uint32_t pick; };
    std::vector<Op> ops(1 << 20);
    for (auto& o : ops) {
        const auto r = rng() % 100;
        o.kind = r < 60 ? 0 : r < 70 ? 1 : r < 75 ? 2 : 3;
        o.side = rng() % 2 ? Side::Buy : Side::Sell;
        o.px = static_cast<std::uint32_t>(rng() % 20);
        o.qty = static_cast<std::uint32_t>(1 + rng() % 100);
        o.pick = static_cast<std::uint32_t>(rng());
    }
    std::size_t i = 0;
    for (auto _ : state) {
        const Op& o = ops[i++ & (ops.size() - 1)];
        trades.clear();
        switch (o.kind) {
            case 0: {  // passive: rests away from the mid (1000)
                const std::uint32_t px = o.side == Side::Buy ? 999 - o.px : 1001 + o.px;
                e.limit(next, o.side, px, o.qty, Tif::GTC, trades);
                ids.push_back(next++);
                break;
            }
            case 1: {  // aggressive limit through the touch
                const std::uint32_t px = o.side == Side::Buy ? 1003 : 997;
                e.limit(next++, o.side, px, o.qty, Tif::GTC, trades);
                break;
            }
            case 2:
                e.market(next++, o.side, o.qty, trades);
                break;
            default:
                if (!ids.empty()) {
                    const std::size_t k = o.pick % ids.size();
                    e.cancel(ids[k]);  // may already be filled: then a cheap miss
                    ids[k] = ids.back();
                    ids.pop_back();
                }
        }
        benchmark::DoNotOptimize(trades.data());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_MatchingEngineMixedFlow);

// Feed book: add then delete, keeping ~2,000 resting orders on one side.
// Arg 0: prices uniform over 32 levels (std::map's best case: small, in-cache tree).
// Arg 1: prices near the top (geometric: half at the best level, a quarter one
//        level down, ...), which is how real feeds behave.
template <class Book>
static void BM_FeedBookAddDelete(benchmark::State& state) {
    Book b(1 << 20);
    std::mt19937 rng(2);
    std::geometric_distribution<int> geo(0.5);
    std::vector<std::uint32_t> px(1 << 16);
    for (auto& p : px) {
        const int lvl = state.range(0) == 0 ? static_cast<int>(rng() % 32) : std::min(geo(rng), 31);
        p = 1'003'100 - static_cast<std::uint32_t>(lvl) * 100;  // best bid = 1,003,100
    }
    std::uint64_t ref = 1;
    for (int k = 0; k < 2000; ++k) b.add(1, ref++, 'B', px[static_cast<std::size_t>(k)], 100);
    std::size_t i = 2000;
    for (auto _ : state) {
        b.add(1, ref, 'B', px[i++ & 0xffff], 100);
        b.remove(ref - 2000);  // oldest first
        ++ref;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 2);
}
BENCHMARK_TEMPLATE(BM_FeedBookAddDelete, FastBook)->Arg(0)->Arg(1);
BENCHMARK_TEMPLATE(BM_FeedBookAddDelete, BaselineBook)->Arg(0)->Arg(1);
