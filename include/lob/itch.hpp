#pragma once
// Nasdaq TotalView-ITCH 5.0 decoder.
//
// Layouts follow the Nasdaq specification (NQTVITCHspecification.pdf):
// every message starts with an 11-byte header
//   [0] type  [1..2] stock locate  [3..4] tracking number  [5..10] timestamp (ns since midnight)
// All integers are big-endian. Prices are uint32 with 4 implied decimals.
//
// The decoder turns the order-book messages (A F E C X D U, plus P) into a flat
// `Event`. Everything else is classified so callers can count it and skip it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "lob/endian.hpp"

namespace lob::itch {

// ----------------------------------------------------------------- message sizes
// Total length in bytes (including the type byte) of every ITCH 5.0 message.
// 0 = not a known ITCH 5.0 type.
inline constexpr std::array<std::uint8_t, 256> kMessageSize = [] {
    std::array<std::uint8_t, 256> s{};
    s['S'] = 12;  // System Event
    s['R'] = 39;  // Stock Directory
    s['H'] = 25;  // Stock Trading Action
    s['Y'] = 20;  // Reg SHO Restriction
    s['L'] = 26;  // Market Participant Position
    s['V'] = 35;  // MWCB Decline Level
    s['W'] = 12;  // MWCB Status
    s['K'] = 28;  // IPO Quoting Period Update
    s['J'] = 35;  // LULD Auction Collar
    s['h'] = 21;  // Operational Halt
    s['A'] = 36;  // Add Order (no MPID)
    s['F'] = 40;  // Add Order with MPID attribution
    s['E'] = 31;  // Order Executed
    s['C'] = 36;  // Order Executed With Price
    s['X'] = 23;  // Order Cancel
    s['D'] = 19;  // Order Delete
    s['U'] = 35;  // Order Replace
    s['P'] = 44;  // Trade (non-cross)
    s['Q'] = 40;  // Cross Trade
    s['B'] = 19;  // Broken Trade
    s['I'] = 50;  // NOII
    s['N'] = 20;  // Retail Price Improvement Indicator
    s['O'] = 48;  // Direct Listing with Capital Raise Price Discovery
    return s;
}();

inline constexpr std::size_t kHeaderSize = 11;

// ----------------------------------------------------------------- event
enum class EventType : std::uint8_t {
    None = 0,
    Add,           // A, F
    Execute,       // E
    ExecutePrice,  // C (execution at a price other than the order's)
    Cancel,        // X (partial cancel)
    Delete,        // D
    Replace,       // U
    Trade,         // P (non-displayed order executed; no book change)
};

// Flat, trivially-copyable decoded event (40 bytes). Fields not used by a
// message type are zero.
struct alignas(8) Event {
    std::uint64_t ref = 0;      // order reference number (U: original ref)
    std::uint64_t new_ref = 0;  // U: new order reference number
    std::uint64_t ts = 0;       // ns since midnight
    std::uint32_t shares = 0;   // A/F/U: order size; E/C: executed; X: cancelled; P: trade size
    std::uint32_t price = 0;    // A/F/U/P: price; C: execution price  (1e-4 USD)
    std::uint16_t locate = 0;   // stock locate code
    EventType type = EventType::None;
    char side = 0;              // 'B' or 'S' (A/F/P only)
    char msg_type = 0;          // raw ITCH type byte
};
static_assert(sizeof(Event) == 40);

enum class Decode : std::uint8_t {
    BookEvent,  // `ev` filled; affects (or reports on) the order book
    Other,      // valid ITCH message that the book ignores (S, R, H, ...)
    Unknown,    // unknown type byte
    BadLength,  // length disagrees with the spec for this type
};

// Decode one message (without its framing length prefix).
inline Decode decode(const std::uint8_t* p, std::size_t len, Event& ev) noexcept {
    if (len == 0) return Decode::BadLength;
    const auto t = static_cast<unsigned char>(p[0]);
    const std::size_t expected = kMessageSize[t];
    if (expected == 0) return Decode::Unknown;
    if (len != expected) return Decode::BadLength;

    ev = Event{};
    ev.msg_type = static_cast<char>(t);
    ev.locate = load_be16(p + 1);
    ev.ts = load_be48(p + 5);

    switch (t) {
        case 'A':
        case 'F':
            ev.type = EventType::Add;
            ev.ref = load_be64(p + 11);
            ev.side = static_cast<char>(p[19]);
            ev.shares = load_be32(p + 20);
            ev.price = load_be32(p + 32);
            return Decode::BookEvent;
        case 'E':
            ev.type = EventType::Execute;
            ev.ref = load_be64(p + 11);
            ev.shares = load_be32(p + 19);
            return Decode::BookEvent;
        case 'C':
            ev.type = EventType::ExecutePrice;
            ev.ref = load_be64(p + 11);
            ev.shares = load_be32(p + 19);
            ev.price = load_be32(p + 32);
            return Decode::BookEvent;
        case 'X':
            ev.type = EventType::Cancel;
            ev.ref = load_be64(p + 11);
            ev.shares = load_be32(p + 19);
            return Decode::BookEvent;
        case 'D':
            ev.type = EventType::Delete;
            ev.ref = load_be64(p + 11);
            return Decode::BookEvent;
        case 'U':
            ev.type = EventType::Replace;
            ev.ref = load_be64(p + 11);
            ev.new_ref = load_be64(p + 19);
            ev.shares = load_be32(p + 27);
            ev.price = load_be32(p + 31);
            return Decode::BookEvent;
        case 'P':
            ev.type = EventType::Trade;
            ev.ref = load_be64(p + 11);
            ev.side = static_cast<char>(p[19]);
            ev.shares = load_be32(p + 20);
            ev.price = load_be32(p + 32);
            return Decode::BookEvent;
        default:
            return Decode::Other;
    }
}

// ----------------------------------------------------------------- non-book messages
struct StockDirectory {
    std::uint16_t locate;
    char stock[9];  // 8 chars, right-padded with spaces in the feed; trimmed + NUL here
    std::uint32_t round_lot;
};

inline bool decode_stock_directory(const std::uint8_t* p, std::size_t len,
                                   StockDirectory& out) noexcept {
    if (len != kMessageSize['R'] || p[0] != 'R') return false;
    out.locate = load_be16(p + 1);
    std::memcpy(out.stock, p + 11, 8);
    int n = 8;
    while (n > 0 && out.stock[n - 1] == ' ') --n;
    out.stock[n] = '\0';
    out.round_lot = load_be32(p + 21);
    return true;
}

// System event code ('O' start of messages, 'S' start of system hours, 'Q' start of
// market hours, 'M' end of market hours, 'E' end of system hours, 'C' end of messages).
inline char system_event_code(const std::uint8_t* p, std::size_t len) noexcept {
    return (len == kMessageSize['S'] && p[0] == 'S') ? static_cast<char>(p[11]) : '\0';
}

// Stock symbol (8 chars, space padded) of an Add message, as a view into the buffer.
inline std::string_view add_stock(const std::uint8_t* p) noexcept {
    return {reinterpret_cast<const char*>(p + 24), 8};
}

}  // namespace lob::itch
