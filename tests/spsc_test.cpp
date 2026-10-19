#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "lob/spsc.hpp"

using lob::SpscRing;

TEST(Spsc, FifoFullEmptyAndWraparound) {
    auto r = std::make_unique<SpscRing<int, 4>>();
    int v = 0;
    EXPECT_FALSE(r->try_pop(v));
    for (int round = 0; round < 10; ++round) {           // indices wrap many times
        for (int i = 0; i < 4; ++i) EXPECT_TRUE(r->try_push(round * 10 + i));
        EXPECT_FALSE(r->try_push(99)) << "ring of 4 accepted a fifth item";
        EXPECT_EQ(r->size(), 4u);
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(r->try_pop(v));
            EXPECT_EQ(v, round * 10 + i);
        }
        EXPECT_FALSE(r->try_pop(v));
    }
}

TEST(Spsc, PopBatch) {
    auto r = std::make_unique<SpscRing<int, 8>>();
    for (int i = 0; i < 6; ++i) r->try_push(i);
    int out[8];
    EXPECT_EQ(r->pop_batch(out, 4), 4u);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(out[i], i);
    for (int i = 6; i < 10; ++i) EXPECT_TRUE(r->try_push(i));   // wraps
    EXPECT_EQ(r->pop_batch(out, 8), 6u);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(out[i], i + 4);
    EXPECT_EQ(r->pop_batch(out, 8), 0u);
}

// Two threads, every item must arrive exactly once and in order.
template <class Ring>
void stress(std::uint64_t n, bool batch) {
    auto r = std::make_unique<Ring>();
    std::thread prod([&] {
        for (std::uint64_t i = 0; i < n;)
            if (r->try_push(i)) ++i;
    });
    std::uint64_t expect = 0;
    std::uint64_t buf[64];
    while (expect < n) {
        if (batch) {
            std::size_t k = r->pop_batch(buf, 64);
            for (std::size_t i = 0; i < k; ++i) ASSERT_EQ(buf[i], expect++);
        } else {
            std::uint64_t v;
            if (r->try_pop(v)) ASSERT_EQ(v, expect++);
        }
    }
    prod.join();
    std::uint64_t v;
    EXPECT_FALSE(r->try_pop(v));
}

TEST(Spsc, TwoThreadsInOrder) { stress<SpscRing<std::uint64_t, 1024>>(5'000'000, false); }
TEST(Spsc, TwoThreadsBatched) { stress<SpscRing<std::uint64_t, 1024>>(5'000'000, true); }
TEST(Spsc, TwoThreadsSmallRing) { stress<SpscRing<std::uint64_t, 2>>(1'000'000, false); }
TEST(Spsc, TwoThreadsNoCacheNoPad) { stress<SpscRing<std::uint64_t, 1024, false, false>>(2'000'000, false); }
