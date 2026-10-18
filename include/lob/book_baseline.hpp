#pragma once
// Baseline book built from standard containers, used as the speed comparison point:
//   std::unordered_map<ref, OrderRec>  +  per stock std::map<price, qty> per side.
// Same interface and semantics as FastBook.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/book_common.hpp"

namespace lob {

class BaselineBook {
   public:
    static constexpr const char* kName = "baseline";

    explicit BaselineBook(std::size_t expected_orders = 1 << 20) {
        orders_.reserve(expected_orders);
    }

    int add(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t price,
            std::uint32_t qty) {
        if (side != 'B' && side != 'S') {
            ++counters_.bad_side;
            return -1;
        }
        if (orders_.count(ref)) {
            ++counters_.duplicate_ref;
            remove(ref);
        }
        orders_.emplace(ref, OrderRec{price, qty, locate, side});
        Stock& s = stock(locate);
        if (side == 'B') s.bids[price] += qty;
        else s.asks[price] += qty;
        return locate;
    }

    int reduce(std::uint64_t ref, std::uint32_t qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++counters_.unknown_ref;
            return -1;
        }
        OrderRec& o = it->second;
        const std::uint32_t take = qty < o.qty ? qty : o.qty;
        const int loc = o.locate;
        take_from_level(o, take);
        o.qty -= take;
        if (o.qty == 0) orders_.erase(it);
        return loc;
    }

    int remove(std::uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++counters_.unknown_ref;
            return -1;
        }
        const int loc = it->second.locate;
        take_from_level(it->second, it->second.qty);
        orders_.erase(it);
        return loc;
    }

    int replace(std::uint64_t ref, std::uint64_t new_ref, std::uint32_t price, std::uint32_t qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++counters_.unknown_ref;
            return -1;
        }
        const OrderRec old = it->second;
        take_from_level(old, old.qty);
        orders_.erase(it);
        return add(old.locate, new_ref, old.side, price, qty);
    }

    Bbo bbo(std::uint16_t locate) const {
        Bbo b;
        if (locate >= stocks_.size()) return b;
        const Stock& s = stocks_[locate];
        if (!s.bids.empty()) {
            b.bid_px = s.bids.begin()->first;
            b.bid_qty = s.bids.begin()->second;
        }
        if (!s.asks.empty()) {
            b.ask_px = s.asks.begin()->first;
            b.ask_qty = s.asks.begin()->second;
        }
        return b;
    }

    template <class Out>
    void depth(std::uint16_t locate, char side, std::size_t n, Out out) const {
        if (locate >= stocks_.size()) return;
        const Stock& s = stocks_[locate];
        std::size_t k = 0;
        if (side == 'B') {
            for (auto it = s.bids.begin(); it != s.bids.end() && k < n; ++it, ++k)
                out(LevelView{it->first, it->second});
        } else {
            for (auto it = s.asks.begin(); it != s.asks.end() && k < n; ++it, ++k)
                out(LevelView{it->first, it->second});
        }
    }

    std::size_t live_orders() const noexcept { return orders_.size(); }
    const BookCounters& counters() const noexcept { return counters_; }

   private:
    struct Stock {
        std::map<std::uint32_t, std::uint64_t, std::greater<>> bids;  // best first
        std::map<std::uint32_t, std::uint64_t> asks;                  // best first
    };

    Stock& stock(std::uint16_t locate) {
        if (locate >= stocks_.size()) stocks_.resize(std::size_t{locate} + 1);
        return stocks_[locate];
    }

    void take_from_level(const OrderRec& o, std::uint32_t qty) {
        Stock& s = stocks_[o.locate];
        if (o.side == 'B') {
            auto lv = s.bids.find(o.price);
            if ((lv->second -= qty) == 0) s.bids.erase(lv);
        } else {
            auto lv = s.asks.find(o.price);
            if ((lv->second -= qty) == 0) s.asks.erase(lv);
        }
    }

    std::unordered_map<std::uint64_t, OrderRec> orders_;
    std::vector<Stock> stocks_;
    BookCounters counters_;
};

}  // namespace lob
