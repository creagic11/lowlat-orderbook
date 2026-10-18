#pragma once
// Types shared by both order book implementations.

#include <cstdint>

namespace lob {

// Best bid and offer of one stock. A missing side has price 0 and quantity 0.
struct Bbo {
    std::uint32_t bid_px = 0;
    std::uint32_t ask_px = 0;
    std::uint64_t bid_qty = 0;
    std::uint64_t ask_qty = 0;
    friend bool operator==(const Bbo&, const Bbo&) = default;
};

// One price level in a depth snapshot.
struct LevelView {
    std::uint32_t price = 0;
    std::uint64_t qty = 0;
    friend bool operator==(const LevelView&, const LevelView&) = default;
};

// What the book keeps per resting order (the feed tells us which order traded,
// so no per-level FIFO is needed to maintain the book).
struct OrderRec {
    std::uint32_t price = 0;
    std::uint32_t qty = 0;
    std::uint16_t locate = 0;
    char side = 0;  // 'B' or 'S'
};

struct BookCounters {
    std::uint64_t unknown_ref = 0;    // E/C/X/D/U for an order we never saw
    std::uint64_t duplicate_ref = 0;  // A/F/U reusing a live reference number
    std::uint64_t bad_side = 0;       // side byte other than 'B'/'S'
};

// Binary BBO record, identical to tools/ref_book.py (little-endian, 40 bytes).
struct BboRecord {
    std::uint64_t seq;
    std::uint64_t bid_qty;
    std::uint64_t ask_qty;
    std::uint32_t bid_px;
    std::uint32_t ask_px;
    std::uint16_t locate;
    std::uint8_t pad[6];
};
static_assert(sizeof(BboRecord) == 40);

}  // namespace lob
