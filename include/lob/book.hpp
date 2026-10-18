#pragma once
// Optimised multi-stock order book for a market-data feed.
//
//   orders: OrderMap<OrderRec>   ref -> {price, qty, locate, side}, stored inline
//   levels: per stock, per side, a std::vector<Level> sorted worst -> best,
//           so the best price is always back() and BBO reads are O(1).
//
// Almost all feed activity happens at or near the top of the book, so finding a
// level scans a few entries back from the best price. Deep lookups fall back to
// binary search. Inserting or erasing a level near the top moves only the few
// levels above it. Memory grows with the number of active levels, so far-away
// outlier prices need no special handling.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/book_common.hpp"
#include "lob/order_map.hpp"

namespace lob {

class SideLevels {
   public:
    struct Level {
        std::uint32_t price;
        std::uint32_t orders;
        std::uint64_t qty;
    };

    explicit SideLevels(bool is_bid = true) : is_bid_(is_bid) {}

    bool empty() const noexcept { return v_.empty(); }
    std::size_t size() const noexcept { return v_.size(); }
    const Level& best() const noexcept { return v_.back(); }

    void add(std::uint32_t price, std::uint64_t qty) {
        const std::size_t i = lower(price);
        if (i < v_.size() && v_[i].price == price) {
            v_[i].qty += qty;
            ++v_[i].orders;
        } else {
            v_.insert(v_.begin() + static_cast<std::ptrdiff_t>(i), Level{price, 1, qty});
        }
    }

    // Removes `qty` shares at `price`; `order_gone` if the order left the level.
    void reduce(std::uint32_t price, std::uint64_t qty, bool order_gone) noexcept {
        const std::size_t i = lower(price);
        Level& l = v_[i];  // order exists, so its level exists
        l.qty -= qty;
        if (order_gone) --l.orders;
        if (l.orders == 0) v_.erase(v_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // Best `n` levels, best first.
    template <class Out>
    void depth(std::size_t n, Out out) const {
        for (std::size_t k = 0; k < n && k < v_.size(); ++k) {
            const Level& l = v_[v_.size() - 1 - k];
            out(LevelView{l.price, l.qty});
        }
    }

   private:
    // true if price a is a worse price than b for this side.
    bool worse(std::uint32_t a, std::uint32_t b) const noexcept { return is_bid_ ? a < b : a > b; }

    // First index whose price is not worse than `price` (v_ is sorted worst -> best).
    std::size_t lower(std::uint32_t price) const noexcept {
        std::size_t i = v_.size();
        constexpr std::size_t kScan = 8;
        std::size_t scanned = 0;
        while (i > 0 && !worse(v_[i - 1].price, price)) {
            --i;
            if (++scanned == kScan && i > 0 && !worse(v_[i - 1].price, price)) {
                // Deep in the book: binary search the remaining prefix.
                auto it = std::partition_point(
                    v_.begin(), v_.begin() + static_cast<std::ptrdiff_t>(i),
                    [&](const Level& l) { return worse(l.price, price); });
                return static_cast<std::size_t>(it - v_.begin());
            }
        }
        return i;
    }

    std::vector<Level> v_;
    bool is_bid_;
};

class FastBook {
   public:
    static constexpr const char* kName = "fast";

    explicit FastBook(std::size_t expected_orders = 1 << 20) : orders_(expected_orders) {}

    // Each mutator returns the locate of the stock it changed, or -1.
    int add(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t price,
            std::uint32_t qty) {
        if (side != 'B' && side != 'S') {
            ++counters_.bad_side;
            return -1;
        }
        if (orders_.find(ref)) {  // reused live ref: drop the stale order first
            ++counters_.duplicate_ref;
            remove(ref);
        }
        orders_.insert(ref, OrderRec{price, qty, locate, side});
        stock(locate).side(side).add(price, qty);
        return locate;
    }

    int reduce(std::uint64_t ref, std::uint32_t qty) {
        OrderRec* o = orders_.find(ref);
        if (!o) {
            ++counters_.unknown_ref;
            return -1;
        }
        const std::uint32_t take = qty < o->qty ? qty : o->qty;
        const bool gone = (take == o->qty);
        const int loc = o->locate;
        stock(o->locate).side(o->side).reduce(o->price, take, gone);
        if (gone) orders_.erase(ref);
        else o->qty -= take;
        return loc;
    }

    int remove(std::uint64_t ref) {
        OrderRec* o = orders_.find(ref);
        if (!o) {
            ++counters_.unknown_ref;
            return -1;
        }
        const int loc = o->locate;
        stock(o->locate).side(o->side).reduce(o->price, o->qty, true);
        orders_.erase(ref);
        return loc;
    }

    int replace(std::uint64_t ref, std::uint64_t new_ref, std::uint32_t price, std::uint32_t qty) {
        OrderRec* o = orders_.find(ref);
        if (!o) {
            ++counters_.unknown_ref;
            return -1;
        }
        const OrderRec old = *o;
        stock(old.locate).side(old.side).reduce(old.price, old.qty, true);
        orders_.erase(ref);
        return add(old.locate, new_ref, old.side, price, qty);
    }

    Bbo bbo(std::uint16_t locate) const noexcept {
        Bbo b;
        if (locate >= stocks_.size()) return b;
        const Stock& s = stocks_[locate];
        if (!s.bids.empty()) {
            b.bid_px = s.bids.best().price;
            b.bid_qty = s.bids.best().qty;
        }
        if (!s.asks.empty()) {
            b.ask_px = s.asks.best().price;
            b.ask_qty = s.asks.best().qty;
        }
        return b;
    }

    template <class Out>
    void depth(std::uint16_t locate, char side, std::size_t n, Out out) const {
        if (locate >= stocks_.size()) return;
        const Stock& s = stocks_[locate];
        (side == 'B' ? s.bids : s.asks).depth(n, out);
    }

    std::size_t live_orders() const noexcept { return orders_.size(); }
    const BookCounters& counters() const noexcept { return counters_; }

   private:
    struct Stock {
        SideLevels bids{true};
        SideLevels asks{false};
        SideLevels& side(char s) noexcept { return s == 'B' ? bids : asks; }
    };

    Stock& stock(std::uint16_t locate) {
        if (locate >= stocks_.size()) stocks_.resize(std::size_t{locate} + 1);
        return stocks_[locate];
    }

    OrderMap<OrderRec> orders_;
    std::vector<Stock> stocks_;
    BookCounters counters_;
};

}  // namespace lob
