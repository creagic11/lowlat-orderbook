#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

std::vector<std::pair<std::uint64_t, std::uint32_t>> queue_of(const MatchingEngine& e, Side s,
                                                               std::uint32_t px) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> q;
    e.queue(s, px, [&](std::uint64_t id, std::uint32_t qty) { q.emplace_back(id, qty); });
    return q;
}

}  // namespace

TEST(Matching, RestsWhenNotCrossing) {
    MatchingEngine e;
    std::vector<Trade> t;
    auto r = e.limit(1, Side::Buy, 100, 10, Tif::GTC, t);
    EXPECT_EQ(r.rested, 10u);
    e.limit(2, Side::Sell, 101, 5, Tif::GTC, t);
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(e.bbo(), (Bbo{100, 101, 10, 5}));
}

TEST(Matching, PriceThenTimePriority) {
    MatchingEngine e;
    std::vector<Trade> t;
    e.limit(1, Side::Sell, 101, 5, Tif::GTC, t);
    e.limit(2, Side::Sell, 100, 5, Tif::GTC, t);  // better price, later
    e.limit(3, Side::Sell, 100, 5, Tif::GTC, t);  // same price, later still
    auto r = e.limit(9, Side::Buy, 101, 12, Tif::GTC, t);
    EXPECT_EQ(r.filled, 12u);
    EXPECT_EQ(r.rested, 0u);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0], (Trade{9, 2, 100, 5}));  // best price first
    EXPECT_EQ(t[1], (Trade{9, 3, 100, 5}));  // then time priority
    EXPECT_EQ(t[2], (Trade{9, 1, 101, 2}));  // trades at the resting price
    EXPECT_EQ(e.bbo(), (Bbo{0, 101, 0, 3}));
}

TEST(Matching, PartialFillRestsRemainder) {
    MatchingEngine e;
    std::vector<Trade> t;
    e.limit(1, Side::Sell, 100, 4, Tif::GTC, t);
    auto r = e.limit(2, Side::Buy, 100, 10, Tif::GTC, t);
    EXPECT_EQ(r.filled, 4u);
    EXPECT_EQ(r.rested, 6u);
    EXPECT_EQ(e.bbo(), (Bbo{100, 0, 6, 0}));
    EXPECT_FALSE(e.crossed());
}

TEST(Matching, IocAndMarketNeverRest) {
    MatchingEngine e;
    std::vector<Trade> t;
    e.limit(1, Side::Sell, 100, 3, Tif::GTC, t);
    e.limit(2, Side::Sell, 105, 3, Tif::GTC, t);
    auto ioc = e.limit(3, Side::Buy, 101, 10, Tif::IOC, t);
    EXPECT_EQ(ioc.filled, 3u);
    EXPECT_EQ(ioc.rested, 0u);
    EXPECT_EQ(e.bbo().bid_px, 0u);
    auto mkt = e.market(4, Side::Buy, 10, t);
    EXPECT_EQ(mkt.filled, 3u);  // sweeps whatever is there
    EXPECT_EQ(e.resting_orders(), 0u);
}

TEST(Matching, CancelRemovesFromQueue) {
    MatchingEngine e;
    std::vector<Trade> t;
    e.limit(1, Side::Buy, 100, 1, Tif::GTC, t);
    e.limit(2, Side::Buy, 100, 2, Tif::GTC, t);
    e.limit(3, Side::Buy, 100, 3, Tif::GTC, t);
    EXPECT_TRUE(e.cancel(2));
    EXPECT_FALSE(e.cancel(2));
    EXPECT_EQ(queue_of(e, Side::Buy, 100), (std::vector<std::pair<std::uint64_t, std::uint32_t>>{{1, 1}, {3, 3}}));
    EXPECT_EQ(e.bbo().bid_qty, 4u);
    EXPECT_TRUE(e.cancel(1));
    EXPECT_TRUE(e.cancel(3));
    EXPECT_EQ(e.bbo(), Bbo{});
}

TEST(Matching, ModifyKeepsOrLosesPriority) {
    MatchingEngine e;
    std::vector<Trade> t;
    e.limit(1, Side::Sell, 100, 10, Tif::GTC, t);
    e.limit(2, Side::Sell, 100, 10, Tif::GTC, t);
    EXPECT_TRUE(e.modify(1, 100, 4, t));  // smaller, same price: stays first
    EXPECT_EQ(queue_of(e, Side::Sell, 100).front().first, 1u);
    EXPECT_TRUE(e.modify(1, 100, 8, t));  // larger: goes to the back
    EXPECT_EQ(queue_of(e, Side::Sell, 100).front().first, 2u);
    e.limit(3, Side::Buy, 99, 5, Tif::GTC, t);
    EXPECT_TRUE(e.modify(3, 100, 5, t));  // price change that crosses: trades now
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0], (Trade{3, 2, 100, 5}));
    EXPECT_FALSE(e.modify(42, 100, 1, t));
}

TEST(Matching, RejectsBadOrders) {
    MatchingEngine e;
    std::vector<Trade> t;
    EXPECT_EQ(e.limit(1, Side::Buy, 100, 0, Tif::GTC, t).status, OrderResult::Rejected);
    EXPECT_EQ(e.limit(1, Side::Buy, 0, 5, Tif::GTC, t).status, OrderResult::Rejected);
    e.limit(1, Side::Buy, 100, 5, Tif::GTC, t);
    EXPECT_EQ(e.limit(1, Side::Buy, 100, 5, Tif::GTC, t).status, OrderResult::Rejected);  // duplicate id
}

// ------------------------------------------------------------------ randomised test
// A deliberately naive matcher: a flat list of resting orders, best match found by
// scanning everything. Slow and obviously right; the engine must agree with it on
// every trade and on the book after every operation.
class NaiveMatcher {
   public:
    struct O { std::uint64_t id; Side side; std::uint32_t price, qty; std::uint64_t seq; };

    void limit(std::uint64_t id, Side s, std::uint32_t px, std::uint32_t q, Tif tif, std::vector<Trade>& t) {
        if (q == 0 || px == 0 || find(id) != book_.end()) return;
        q = match(id, s, px, q, t);
        if (q > 0 && tif == Tif::GTC) book_.push_back({id, s, px, q, seq_++});
    }
    void market(std::uint64_t id, Side s, std::uint32_t q, std::vector<Trade>& t) {
        if (q == 0 || find(id) != book_.end()) return;
        match(id, s, s == Side::Buy ? ~0u : 0u, q, t);
    }
    bool cancel(std::uint64_t id) {
        auto it = find(id);
        if (it == book_.end()) return false;
        book_.erase(it);
        return true;
    }
    bool modify(std::uint64_t id, std::uint32_t px, std::uint32_t q, std::vector<Trade>& t) {
        auto it = find(id);
        if (it == book_.end()) return false;
        if (q == 0) return cancel(id);
        if (px == it->price && q <= it->qty) {
            it->qty = q;
            return true;
        }
        const Side s = it->side;
        book_.erase(it);
        limit(id, s, px, q, Tif::GTC, t);
        return true;
    }
    std::map<std::uint32_t, std::uint64_t> levels(Side s) const {
        std::map<std::uint32_t, std::uint64_t> m;
        for (const auto& o : book_)
            if (o.side == s) m[o.price] += o.qty;
        return m;
    }
    std::vector<std::uint64_t> ids() const {
        std::vector<std::uint64_t> v;
        for (const auto& o : book_) v.push_back(o.id);
        return v;
    }

   private:
    std::vector<O>::iterator find(std::uint64_t id) {
        return std::find_if(book_.begin(), book_.end(), [&](const O& o) { return o.id == id; });
    }
    std::uint32_t match(std::uint64_t id, Side s, std::uint32_t px, std::uint32_t q, std::vector<Trade>& t) {
        while (q > 0) {
            auto best = book_.end();
            for (auto it = book_.begin(); it != book_.end(); ++it) {
                if (it->side == s) continue;
                const bool crosses = s == Side::Buy ? it->price <= px : it->price >= px;
                if (!crosses) continue;
                if (best == book_.end()) { best = it; continue; }
                const bool better = s == Side::Buy ? it->price < best->price : it->price > best->price;
                if (better || (it->price == best->price && it->seq < best->seq)) best = it;
            }
            if (best == book_.end()) break;
            const std::uint32_t f = std::min(q, best->qty);
            t.push_back({id, best->id, best->price, f});
            q -= f;
            best->qty -= f;
            if (best->qty == 0) book_.erase(best);
        }
        return q;
    }
    std::vector<O> book_;
    std::uint64_t seq_ = 0;
};

TEST(Matching, RandomisedAgainstNaiveMatcher) {
    std::mt19937 rng(2026);
    MatchingEngine e(64);  // small: forces the order pool and id table to grow
    NaiveMatcher n;
    std::vector<std::uint64_t> live;
    std::uint64_t next_id = 1;
    std::size_t total_trades = 0, modifies = 0, cancels = 0;
    for (int step = 0; step < 40'000; ++step) {
        std::vector<Trade> te, tn;
        const auto r = rng() % 100;
        const auto side = (rng() % 2) ? Side::Buy : Side::Sell;
        const auto px = static_cast<std::uint32_t>(990 + rng() % 21);  // narrow band: lots of crossing
        const auto qty = static_cast<std::uint32_t>(1 + rng() % 20);
        if (r < 55 || live.empty()) {
            const Tif tif = (rng() % 10 == 0) ? Tif::IOC : Tif::GTC;
            e.limit(next_id, side, px, qty, tif, te);
            n.limit(next_id, side, px, qty, tif, tn);
            ++next_id;
        } else if (r < 62) {
            e.market(next_id, side, qty, te);
            n.market(next_id, side, qty, tn);
            ++next_id;
        } else if (r < 85) {
            const auto id = live[rng() % live.size()];
            ASSERT_EQ(e.cancel(id), n.cancel(id));
            ++cancels;
        } else {
            const auto id = live[rng() % live.size()];
            const auto new_qty = static_cast<std::uint32_t>(rng() % 25);
            const auto new_px = (rng() % 2) ? px : 0;  // 0 here = keep the current price below
            std::uint32_t use_px = new_px;
            if (use_px == 0) {  // same-price modify: find the order's price from the engine's view
                for (Side s : {Side::Buy, Side::Sell})
                    for (std::uint32_t p = 990; p <= 1010; ++p)
                        for (auto& [qid, q] : queue_of(e, s, p))
                            if (qid == id) use_px = p;
            }
            ASSERT_EQ(e.modify(id, use_px, new_qty, te), n.modify(id, use_px, new_qty, tn));
            ++modifies;
        }
        total_trades += te.size();
        ASSERT_EQ(te, tn) << "trades differ at step " << step;
        ASSERT_FALSE(e.crossed()) << "book crossed at step " << step;
        if (step % 200 == 0) {
            for (Side s : {Side::Buy, Side::Sell}) {
                std::map<std::uint32_t, std::uint64_t> got;
                e.depth(s, 1000, [&](LevelView l) { got[l.price] = l.qty; });
                ASSERT_EQ(got, n.levels(s)) << "levels differ at step " << step;
            }
            ASSERT_EQ(e.resting_orders(), n.ids().size());
        }
        live = n.ids();
    }
    // Guard against a test that passes because nothing happened.
    EXPECT_GT(total_trades, 10'000u);
    EXPECT_GT(modifies, 3'000u);
    EXPECT_GT(cancels, 5'000u);
    std::printf("randomised: %zu trades, %zu modifies, %zu cancels\n", total_trades, modifies, cancels);
}
