#include <gtest/gtest.h>

#include <memory>

#include "lob/latency.hpp"

using lob::LatencyHistogram;

TEST(Latency, ExactBelow16us) {
    auto h = std::make_unique<LatencyHistogram>();
    for (std::uint64_t v = 1; v <= 1000; ++v) h->record(v);    // 1..1000 ns
    EXPECT_EQ(h->count(), 1000u);
    EXPECT_EQ(h->min(), 1u);
    EXPECT_EQ(h->max(), 1000u);
    EXPECT_EQ(h->percentile(0.50), 500u);
    EXPECT_EQ(h->percentile(0.99), 990u);
    EXPECT_EQ(h->percentile(1.00), 1000u);
    EXPECT_DOUBLE_EQ(h->mean(), 500.5);
}

TEST(Latency, LargeValuesWithinOnePercent) {
    auto h = std::make_unique<LatencyHistogram>();
    const std::uint64_t vals[] = {20'000, 123'456, 5'000'000, 987'654'321};
    for (auto v : vals) {
        auto one = std::make_unique<LatencyHistogram>();
        one->record(v);
        one->record(v - 1);                      // so the max does not clamp the bucket bound
        one->record(v - 1);
        const std::uint64_t p = one->percentile(0.5);
        EXPECT_GE(p, v - 1);
        EXPECT_LE(static_cast<double>(p), static_cast<double>(v) * 1.01) << v;
        h->record(v);
    }
    EXPECT_EQ(h->max(), 987'654'321u);
    EXPECT_EQ(h->percentile(1.0), 987'654'321u);
}

TEST(Latency, MergeAndTsc) {
    auto a = std::make_unique<LatencyHistogram>(), b = std::make_unique<LatencyHistogram>();
    a->record(10);
    b->record(30);
    a->merge(*b);
    EXPECT_EQ(a->count(), 2u);
    EXPECT_EQ(a->max(), 30u);
    lob::TscClock clk(20);
    const std::uint64_t t0 = clk.now();
    const std::uint64_t s0 = lob::steady_ns();
    while (lob::steady_ns() - s0 < 2'000'000) {}  // ~2 ms
    const std::uint64_t dt = clk.to_ns(clk.now() - t0);
    EXPECT_GT(dt, 1'500'000u);
    EXPECT_LT(dt, 4'000'000u);
}
