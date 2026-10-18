#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_baseline.hpp"
#include "lob/book_builder.hpp"
#include "lob/itch.hpp"
#include "lob/itch_file.hpp"

using namespace lob;

// Every test runs against both books: they must behave identically.
template <class B>
class BookTest : public ::testing::Test {
   protected:
    B book{1024};
    std::vector<LevelView> depth(std::uint16_t loc, char side, std::size_t n = 10) {
        std::vector<LevelView> v;
        book.depth(loc, side, n, [&](LevelView l) { v.push_back(l); });
        return v;
    }
};
using Books = ::testing::Types<FastBook, BaselineBook>;
TYPED_TEST_SUITE(BookTest, Books);

TYPED_TEST(BookTest, EmptyBookHasNoBbo) {
    EXPECT_EQ(this->book.bbo(1), Bbo{});
    EXPECT_EQ(this->book.bbo(60000), Bbo{});
}

TYPED_TEST(BookTest, AddsBuildLevelsAndBbo) {
    auto& b = this->book;
    EXPECT_EQ(b.add(1, 100, 'B', 1000'000, 100), 1);
    b.add(1, 101, 'B', 1000'000, 50);   // same level
    b.add(1, 102, 'B', 999'900, 200);   // worse bid
    b.add(1, 103, 'S', 1000'200, 300);
    b.add(1, 104, 'S', 1000'100, 10);   // better ask
    EXPECT_EQ(b.bbo(1), (Bbo{1000'000, 1000'100, 150, 10}));
    EXPECT_EQ(this->depth(1, 'B'),
              (std::vector<LevelView>{{1000'000, 150}, {999'900, 200}}));
    EXPECT_EQ(this->depth(1, 'S'),
              (std::vector<LevelView>{{1000'100, 10}, {1000'200, 300}}));
    EXPECT_EQ(b.live_orders(), 5u);
}

TYPED_TEST(BookTest, ExecutionsAndCancelsReduceThenRemove) {
    auto& b = this->book;
    b.add(7, 1, 'S', 500'000, 100);
    b.add(7, 2, 'S', 500'000, 100);
    EXPECT_EQ(b.reduce(1, 30), 7);     // partial execution
    EXPECT_EQ(b.bbo(7).ask_qty, 170u);
    EXPECT_EQ(b.reduce(1, 70), 7);     // order 1 gone
    EXPECT_EQ(b.live_orders(), 1u);
    EXPECT_EQ(b.reduce(2, 500), 7);    // over-execution is clamped
    EXPECT_EQ(b.bbo(7), Bbo{});
    EXPECT_EQ(b.live_orders(), 0u);
}

TYPED_TEST(BookTest, DeleteEmptiesBestLevelAndNextBestTakesOver) {
    auto& b = this->book;
    b.add(2, 10, 'B', 2000'000, 100);
    b.add(2, 11, 'B', 1999'900, 100);
    b.add(2, 12, 'B', 1999'800, 100);
    EXPECT_EQ(b.remove(10), 2);
    EXPECT_EQ(b.bbo(2).bid_px, 1999'900u);
    EXPECT_EQ(b.remove(11), 2);
    EXPECT_EQ(b.bbo(2).bid_px, 1999'800u);
}

TYPED_TEST(BookTest, ReplaceMovesOrderAndKeepsSideAndStock) {
    auto& b = this->book;
    b.add(3, 20, 'S', 300'000, 100);
    b.add(3, 21, 'S', 300'100, 100);
    EXPECT_EQ(b.replace(20, 22, 300'200, 400), 3);
    EXPECT_EQ(b.bbo(3), (Bbo{0, 300'100, 0, 100}));
    EXPECT_EQ(b.reduce(20, 1), -1);  // old ref is gone
    EXPECT_EQ(b.reduce(22, 100), 3);  // new ref works, on the ask side
    EXPECT_EQ(this->depth(3, 'S'), (std::vector<LevelView>{{300'100, 100}, {300'200, 300}}));
}

TYPED_TEST(BookTest, UnknownAndDuplicateRefsAreCountedNotFatal) {
    auto& b = this->book;
    EXPECT_EQ(b.reduce(999, 1), -1);
    EXPECT_EQ(b.remove(999), -1);
    EXPECT_EQ(b.replace(999, 1000, 1, 1), -1);
    EXPECT_EQ(b.counters().unknown_ref, 3u);
    b.add(4, 50, 'B', 100'000, 100);
    b.add(4, 50, 'B', 100'100, 200);  // reused live ref: old order dropped
    EXPECT_EQ(b.counters().duplicate_ref, 1u);
    EXPECT_EQ(b.bbo(4), (Bbo{100'100, 0, 200, 0}));
    EXPECT_EQ(b.live_orders(), 1u);
    EXPECT_EQ(b.add(4, 51, 'Z', 1, 1), -1);
    EXPECT_EQ(b.counters().bad_side, 1u);
}

TYPED_TEST(BookTest, StocksAreIndependentAndOutliersWork) {
    auto& b = this->book;
    b.add(1, 1, 'B', 100'000, 100);
    b.add(65535, 2, 'B', 2'000'000'000, 1);  // $200,000 bid on the highest locate
    b.add(65535, 3, 'B', 1, 1);              // $0.0001 bid
    EXPECT_EQ(b.bbo(1).bid_px, 100'000u);
    EXPECT_EQ(b.bbo(65535).bid_px, 2'000'000'000u);
    b.remove(2);
    EXPECT_EQ(b.bbo(65535).bid_px, 1u);
}

// Deep book: forces FastBook's level search past the linear scan into binary search.
TYPED_TEST(BookTest, DeepBookRandomOpsMatchModel) {
    std::mt19937 rng(99);
    std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> live;  // ref -> (px, qty)
    std::map<std::uint32_t, std::uint64_t> model;  // bid levels
    std::uint64_t next = 1;
    for (int i = 0; i < 20'000; ++i) {
        if (live.empty() || rng() % 2) {
            const auto px = static_cast<std::uint32_t>(100'000 + (rng() % 400) * 100);
            const auto q = static_cast<std::uint32_t>(1 + rng() % 500);
            this->book.add(9, next, 'B', px, q);
            live[next++] = {px, q};
            model[px] += q;
        } else {
            auto it = live.begin();
            std::advance(it, rng() % live.size());
            auto [px, q] = it->second;
            if ((model[px] -= q) == 0) model.erase(px);
            this->book.remove(it->first);
            live.erase(it);
        }
        if (i % 500 == 0) {
            auto d = this->depth(9, 'B', 1000);
            ASSERT_EQ(d.size(), model.size());
            auto m = model.rbegin();
            for (const auto& l : d) {
                ASSERT_EQ(l.price, m->first);
                ASSERT_EQ(l.qty, m->second);
                ++m;
            }
        }
    }
}

// The full synthetic day through BookBuilder must reproduce tools/ref_book.py's
// BBO stream byte for byte (tests/data/synth_small.bbo).
template <class B>
std::vector<BboRecord> replay_file(const std::string& path) {
    itch::MappedFile f(path);
    BookBuilder<B> builder(1 << 16);
    std::vector<BboRecord> out;
    std::uint64_t seq = 0;
    itch::for_each_message(f.data(), f.size(), [&](const std::uint8_t* m, std::size_t len) {
        itch::Event ev;
        if (itch::decode(m, len, ev) == itch::Decode::BookEvent)
            builder.on_event(seq, ev, [&](const BboRecord& r) { out.push_back(r); });
        ++seq;
    });
    EXPECT_EQ(builder.book().counters().unknown_ref, 0u);
    return out;
}

TYPED_TEST(BookTest, SyntheticDayMatchesReferenceBboStream) {
    const std::string dir = LOB_TEST_DATA_DIR;
    itch::MappedFile ref(dir + "/synth_small.bbo");
    const auto got = replay_file<TypeParam>(dir + "/synth_small.itch");
    ASSERT_EQ(got.size() * sizeof(BboRecord), ref.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        ASSERT_EQ(std::memcmp(&got[i], ref.data() + i * sizeof(BboRecord), sizeof(BboRecord)), 0)
            << "first difference at BBO record " << i << " (message " << got[i].seq << ")";
    }
}
