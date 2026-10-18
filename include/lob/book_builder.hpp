#pragma once
// Applies decoded ITCH events to a book and reports BBO changes.
//
// BookBuilder<Book> works with FastBook and BaselineBook alike. After every
// event that changes a stock, it compares that stock's BBO with the last one
// reported and calls on_bbo(BboRecord) only when it changed.

#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/book_common.hpp"
#include "lob/itch.hpp"

namespace lob {

template <class Book>
class BookBuilder {
   public:
    explicit BookBuilder(std::size_t expected_orders = 1 << 20)
        : book_(expected_orders), last_(65536) {}

    // Returns the locate whose book changed, or -1.
    int apply(const itch::Event& ev) {
        using itch::EventType;
        switch (ev.type) {
            case EventType::Add:
                return book_.add(ev.locate, ev.ref, ev.side, ev.price, ev.shares);
            case EventType::Execute:
            case EventType::ExecutePrice:  // the book price is the order's, not the print's
            case EventType::Cancel:
                return book_.reduce(ev.ref, ev.shares);
            case EventType::Delete:
                return book_.remove(ev.ref);
            case EventType::Replace:
                return book_.replace(ev.ref, ev.new_ref, ev.price, ev.shares);
            default:
                return -1;  // P (hidden trade) and anything else: no book change
        }
    }

    // apply() + BBO change detection. `seq` is the message's index in the feed.
    template <class OnBbo>
    void on_event(std::uint64_t seq, const itch::Event& ev, OnBbo&& on_bbo) {
        const int loc = apply(ev);
        if (loc < 0) return;
        const Bbo b = book_.bbo(static_cast<std::uint16_t>(loc));
        Bbo& last = last_[static_cast<std::size_t>(loc)];
        if (b == last) return;
        last = b;
        BboRecord r;
        std::memset(&r, 0, sizeof r);
        r.seq = seq;
        r.bid_qty = b.bid_qty;
        r.ask_qty = b.ask_qty;
        r.bid_px = b.bid_px;
        r.ask_px = b.ask_px;
        r.locate = static_cast<std::uint16_t>(loc);
        ++bbo_updates_;
        on_bbo(r);
    }

    Book& book() noexcept { return book_; }
    const Book& book() const noexcept { return book_; }
    std::uint64_t bbo_updates() const noexcept { return bbo_updates_; }

   private:
    Book book_;
    std::vector<Bbo> last_;
    std::uint64_t bbo_updates_ = 0;
};

}  // namespace lob
