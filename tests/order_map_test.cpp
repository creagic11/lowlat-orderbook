#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "lob/order_map.hpp"

using lob::OrderMap;

TEST(OrderMap, InsertFindErase) {
    OrderMap<int> m(4);
    EXPECT_EQ(m.find(10), nullptr);
    EXPECT_TRUE(m.insert(10, 1).second);
    EXPECT_FALSE(m.insert(10, 2).second);  // existing value kept
    ASSERT_NE(m.find(10), nullptr);
    EXPECT_EQ(*m.find(10), 1);
    EXPECT_TRUE(m.erase(10));
    EXPECT_FALSE(m.erase(10));
    EXPECT_EQ(m.find(10), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderMap, GrowsAndKeepsEverything) {
    OrderMap<std::uint64_t> m(16);
    for (std::uint64_t k = 1; k <= 100'000; ++k) m.insert(k * 7, k);
    EXPECT_EQ(m.size(), 100'000u);
    EXPECT_GE(m.capacity(), 200'000u);
    for (std::uint64_t k = 1; k <= 100'000; ++k) {
        auto* v = m.find(k * 7);
        ASSERT_NE(v, nullptr);
        ASSERT_EQ(*v, k);
    }
}

// Random inserts and erases against std::unordered_map. A small table forces long
// probe runs and wrap-around, which is where backward-shift deletion can go wrong.
template <class H>
class OrderMapHash : public ::testing::Test {};
using Hashes = ::testing::Types<lob::SequentialRefHash, lob::FibonacciHash>;
TYPED_TEST_SUITE(OrderMapHash, Hashes);

TYPED_TEST(OrderMapHash, RandomOpsMatchStdUnorderedMap) {
    std::mt19937_64 rng(12345);
    OrderMap<std::uint64_t, TypeParam> m(64);
    std::unordered_map<std::uint64_t, std::uint64_t> ref;
    for (int step = 0; step < 300'000; ++step) {
        // Mix of dense keys and keys sharing low bits (worst case for the low-bits hash).
        const std::uint64_t key = (rng() % 2) ? rng() % 2000 : (rng() % 64) << 20;
        if (rng() % 3 == 0) {
            ASSERT_EQ(m.erase(key), ref.erase(key) == 1) << "step " << step;
        } else {
            const auto v = static_cast<std::uint64_t>(step);
            const bool ins = m.insert(key, v).second;
            const bool ref_ins = ref.emplace(key, v).second;
            ASSERT_EQ(ins, ref_ins);
        }
        if (step % 1000 == 0) {
            ASSERT_EQ(m.size(), ref.size());
            for (const auto& [k, v] : ref) {
                auto* got = m.find(k);
                ASSERT_NE(got, nullptr) << "lost key " << k << " at step " << step;
                ASSERT_EQ(*got, v);
            }
        }
    }
}
