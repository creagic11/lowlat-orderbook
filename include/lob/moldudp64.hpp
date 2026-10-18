#pragma once
// MoldUDP64 downstream packets (Nasdaq MoldUDP64 specification).
//
//   header: session (10 bytes ASCII) | sequence number of the first message (8, BE)
//           | message count (2, BE)
//   then `count` blocks of [length (2, BE)][message]
//   count 0 = heartbeat (sequence = next expected), 0xFFFF = end of session.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/endian.hpp"

namespace lob::mold {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

struct Header {
    char session[10];
    std::uint64_t seq;
    std::uint16_t count;
};

inline bool parse_header(const std::uint8_t* p, std::size_t n, Header& h) noexcept {
    if (n < kHeaderSize) return false;
    std::memcpy(h.session, p, 10);
    h.seq = load_be64(p + 10);
    h.count = load_be16(p + 18);
    return true;
}

// Calls f(seq, msg, len) for each message block. Returns false if the blocks run
// past the end of the packet (malformed / truncated).
template <class F>
bool for_each_message(const std::uint8_t* p, std::size_t n, const Header& h, F&& f) {
    if (h.count == kEndOfSession) return true;
    std::size_t off = kHeaderSize;
    for (std::uint16_t i = 0; i < h.count; ++i) {
        if (off + 2 > n) return false;
        const std::size_t len = load_be16(p + off);
        if (off + 2 + len > n) return false;
        f(h.seq + i, p + off + 2, len);
        off += 2 + len;
    }
    return true;
}

// Builds packets of at most max_payload bytes from a message stream.
class Packer {
   public:
    explicit Packer(const char (&session)[11], std::uint64_t first_seq = 1,
                    std::size_t max_payload = 1400)
        : max_(max_payload), next_seq_(first_seq) {
        std::memcpy(session_, session, 10);
        reset();
    }

    // True if the message fits in the current packet (it is then appended).
    bool add(const std::uint8_t* msg, std::size_t len) {
        if (count_ > 0 && buf_.size() + 2 + len > max_) return false;
        const std::size_t at = buf_.size();
        buf_.resize(at + 2 + len);
        store_be16(&buf_[at], static_cast<std::uint16_t>(len));
        std::memcpy(&buf_[at + 2], msg, len);
        ++count_;
        return true;
    }

    std::uint16_t count() const noexcept { return count_; }
    std::uint64_t first_seq() const noexcept { return next_seq_; }

    // Finishes the current packet: writes the header and returns the bytes.
    const std::vector<std::uint8_t>& finish() {
        store_be64(&buf_[10], next_seq_);
        store_be16(&buf_[18], count_);
        return buf_;
    }

    // Starts the next packet after finish().
    void next() {
        next_seq_ += count_;
        reset();
    }

    std::vector<std::uint8_t> end_of_session() const {
        std::vector<std::uint8_t> p(kHeaderSize);
        std::memcpy(p.data(), session_, 10);
        store_be64(&p[10], next_seq_);
        store_be16(&p[18], kEndOfSession);
        return p;
    }

   private:
    void reset() {
        buf_.assign(kHeaderSize, 0);
        std::memcpy(buf_.data(), session_, 10);
        count_ = 0;
    }

    char session_[10];
    std::vector<std::uint8_t> buf_;
    std::size_t max_;
    std::uint64_t next_seq_;
    std::uint16_t count_ = 0;
};

}  // namespace lob::mold
