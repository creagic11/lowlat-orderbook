#pragma once
// Price-time priority matching engine for one instrument.
//
// Orders: limit (GTC or IOC), market, cancel, modify.
//   - Incoming orders match against the best opposite price first and, within a
//     price, against the oldest resting order first (FIFO).
//   - Market orders take liquidity up to their size and never rest.
//   - IOC limit orders take what they can at their limit and never rest.
//   - Modify: reducing the quantity at the same price keeps queue priority;
//     changing the price or increasing the quantity loses it (cancel + new order,
//     which can trade immediately if the new price crosses).
//
// Data layout:
//   - Resting orders live in a pooled array (free list, no allocation once warm),
//     linked into a per-level FIFO by 32-bit indices.
//   - OrderMap: client order id -> pool index.
//   - Levels per side: vector sorted worst -> best, best at back().

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/book_common.hpp"
#include "lob/order_map.hpp"

namespace lob {

enum class Side : std::uint8_t { Buy, Sell };
enum class Tif : std::uint8_t { GTC, IOC };

struct Trade {
    std::uint64_t taker_id;
    std::uint64_t maker_id;
    std::uint32_t price;
    std::uint32_t qty;
    friend bool operator==(const Trade&, const Trade&) = default;
};

struct OrderResult {
    enum Status : std::uint8_t { Accepted, Rejected } status = Accepted;
    std::uint32_t filled = 0;  // shares traded on entry
    std::uint32_t rested = 0;  // shares left resting on the book
};

class MatchingEngine {
   public:
    explicit MatchingEngine(std::size_t expected_orders = 1 << 16) : ids_(expected_orders) {
        nodes_.reserve(expected_orders);
    }

    OrderResult limit(std::uint64_t id, Side side, std::uint32_t price, std::uint32_t qty, Tif tif,
                      std::vector<Trade>& trades) {
        if (qty == 0 || price == 0 || ids_.find(id)) return {OrderResult::Rejected, 0, 0};
        const std::uint32_t left = match(id, side, price, qty, trades);
        OrderResult r{OrderResult::Accepted, qty - left, 0};
        if (left > 0 && tif == Tif::GTC) {
            rest(id, side, price, left);
            r.rested = left;
        }
        return r;
    }

    OrderResult market(std::uint64_t id, Side side, std::uint32_t qty, std::vector<Trade>& trades) {
        if (qty == 0 || ids_.find(id)) return {OrderResult::Rejected, 0, 0};
        const std::uint32_t any = (side == Side::Buy) ? ~std::uint32_t{0} : 0;
        const std::uint32_t left = match(id, side, any, qty, trades);
        return {OrderResult::Accepted, qty - left, 0};
    }

    bool cancel(std::uint64_t id) {
        std::uint32_t* idx = ids_.find(id);
        if (!idx) return false;
        unlink_and_free(*idx);
        ids_.erase(id);
        return true;
    }

    // new_qty is the new remaining quantity. new_qty == 0 cancels.
    bool modify(std::uint64_t id, std::uint32_t new_price, std::uint32_t new_qty,
                std::vector<Trade>& trades) {
        std::uint32_t* idx = ids_.find(id);
        if (!idx) return false;
        Node& n = nodes_[*idx];
        if (new_qty == 0) return cancel(id);
        if (new_price == n.price && new_qty <= n.qty) {  // keeps priority
            Level& l = side_of(n.side).level_at(n.price);
            l.qty -= n.qty - new_qty;
            n.qty = new_qty;
            return true;
        }
        const Side s = n.side;
        cancel(id);
        limit(id, s, new_price, new_qty, Tif::GTC, trades);  // loses priority, may trade
        return true;
    }

    Bbo bbo() const noexcept {
        Bbo b;
        if (!bids_.empty()) {
            b.bid_px = bids_.best().price;
            b.bid_qty = bids_.best().qty;
        }
        if (!asks_.empty()) {
            b.ask_px = asks_.best().price;
            b.ask_qty = asks_.best().qty;
        }
        return b;
    }

    bool crossed() const noexcept {
        return !bids_.empty() && !asks_.empty() && bids_.best().price >= asks_.best().price;
    }

    template <class Out>
    void depth(Side side, std::size_t n, Out out) const {
        const SideBook& sb = side == Side::Buy ? bids_ : asks_;
        for (std::size_t k = 0; k < n && k < sb.levels.size(); ++k) {
            const Level& l = sb.levels[sb.levels.size() - 1 - k];
            out(LevelView{l.price, l.qty});
        }
    }

    // Resting order ids at one price, oldest first (for tests and debugging).
    template <class Out>
    void queue(Side side, std::uint32_t price, Out out) const {
        const SideBook& sb = side == Side::Buy ? bids_ : asks_;
        const Level* l = sb.find(price);
        for (std::uint32_t i = l ? l->head : kNil; i != kNil; i = nodes_[i].next) out(nodes_[i].id, nodes_[i].qty);
    }

    std::size_t resting_orders() const noexcept { return ids_.size(); }

   private:
    static constexpr std::uint32_t kNil = ~std::uint32_t{0};

    struct Node {
        std::uint64_t id;
        std::uint32_t price;
        std::uint32_t qty;
        std::uint32_t prev;
        std::uint32_t next;
        Side side;
    };

    struct Level {
        std::uint32_t price;
        std::uint32_t count;
        std::uint64_t qty;
        std::uint32_t head;
        std::uint32_t tail;
    };

    struct SideBook {
        bool is_bid;
        std::vector<Level> levels;  // worst -> best

        bool empty() const noexcept { return levels.empty(); }
        Level& best() noexcept { return levels.back(); }
        const Level& best() const noexcept { return levels.back(); }
        bool worse(std::uint32_t a, std::uint32_t b) const noexcept { return is_bid ? a < b : a > b; }

        std::size_t lower(std::uint32_t price) const noexcept {
            std::size_t i = levels.size();
            for (int k = 0; k < 8 && i > 0 && !worse(levels[i - 1].price, price); ++k) --i;
            if (i > 0 && !worse(levels[i - 1].price, price)) {
                auto it = std::partition_point(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(i),
                                               [&](const Level& l) { return worse(l.price, price); });
                i = static_cast<std::size_t>(it - levels.begin());
            }
            return i;
        }
        const Level* find(std::uint32_t price) const noexcept {
            const std::size_t i = lower(price);
            return (i < levels.size() && levels[i].price == price) ? &levels[i] : nullptr;
        }
        Level& level_at(std::uint32_t price) noexcept { return levels[lower(price)]; }
        Level& find_or_insert(std::uint32_t price) {
            const std::size_t i = lower(price);
            if (i < levels.size() && levels[i].price == price) return levels[i];
            return *levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(i),
                                  Level{price, 0, 0, kNil, kNil});
        }
        void erase_at(std::uint32_t price) {
            levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(lower(price)));
        }
    };

    SideBook& side_of(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }

    // Matches an incoming order; returns the quantity left unfilled.
    std::uint32_t match(std::uint64_t taker, Side side, std::uint32_t limit, std::uint32_t qty,
                        std::vector<Trade>& trades) {
        SideBook& opp = side == Side::Buy ? asks_ : bids_;
        while (qty > 0 && !opp.empty()) {
            Level& l = opp.best();
            const bool crosses = (side == Side::Buy) ? l.price <= limit : l.price >= limit;
            if (!crosses) break;
            std::uint32_t i = l.head;
            while (qty > 0 && i != kNil) {
                Node& m = nodes_[i];
                const std::uint32_t fill = qty < m.qty ? qty : m.qty;
                trades.push_back(Trade{taker, m.id, l.price, fill});
                qty -= fill;
                m.qty -= fill;
                l.qty -= fill;
                const std::uint32_t next = m.next;
                if (m.qty == 0) {
                    ids_.erase(m.id);
                    l.head = next;
                    if (next != kNil) nodes_[next].prev = kNil;
                    else l.tail = kNil;
                    --l.count;
                    free_.push_back(i);
                }
                i = next;
            }
            if (l.count == 0) opp.levels.pop_back();
        }
        return qty;
    }

    void rest(std::uint64_t id, Side side, std::uint32_t price, std::uint32_t qty) {
        std::uint32_t idx;
        if (!free_.empty()) {
            idx = free_.back();
            free_.pop_back();
        } else {
            idx = static_cast<std::uint32_t>(nodes_.size());
            nodes_.emplace_back();
        }
        Level& l = side_of(side).find_or_insert(price);
        nodes_[idx] = Node{id, price, qty, l.tail, kNil, side};
        if (l.tail != kNil) nodes_[l.tail].next = idx;
        else l.head = idx;
        l.tail = idx;
        ++l.count;
        l.qty += qty;
        ids_.insert(id, idx);
    }

    void unlink_and_free(std::uint32_t idx) {
        Node& n = nodes_[idx];
        SideBook& sb = side_of(n.side);
        Level& l = sb.level_at(n.price);
        if (n.prev != kNil) nodes_[n.prev].next = n.next;
        else l.head = n.next;
        if (n.next != kNil) nodes_[n.next].prev = n.prev;
        else l.tail = n.prev;
        l.qty -= n.qty;
        if (--l.count == 0) sb.erase_at(n.price);
        free_.push_back(idx);
    }

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> free_;
    OrderMap<std::uint32_t, FibonacciHash> ids_;  // client ids can be arbitrary
    SideBook bids_{true, {}};
    SideBook asks_{false, {}};
};

}  // namespace lob
