#pragma once
// Open-addressing hash map from order reference number to an inline value.
//
// - Linear probing with Robin Hood ordering over a power-of-two table.
// - Values live inside the table: a lookup is usually a single cache-line access.
// - The slot index comes from a hash policy. The default, SequentialRefHash, takes
//   the low bits of the reference number. Nasdaq assigns order references in
//   increasing order through the day, so the live orders are mostly a sliding
//   window of recent references: low bits put them in neighbouring slots with
//   almost no collisions, and recently added orders (the ones most likely to be
//   executed or cancelled next) share cache lines. FibonacciHash scatters keys
//   instead and is the safe choice for arbitrary keys; both are tested, and the
//   speed difference is recorded in docs/design.md.
// - Deletion uses backward shifting (no tombstones), and Robin Hood ordering lets
//   both lookups and deletions stop early instead of scanning a whole cluster.
// - Load factor is kept <= 1/2; the table doubles when it is exceeded. Callers
//   size it up front (reserve) so growth never happens on the hot path.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lob {

struct SequentialRefHash {
    static std::size_t slot(std::uint64_t key, std::size_t mask, int /*shift*/) noexcept {
        return static_cast<std::size_t>(key) & mask;
    }
};

struct FibonacciHash {
    static std::size_t slot(std::uint64_t key, std::size_t /*mask*/, int shift) noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift);
    }
};

template <class V, class Hash = SequentialRefHash>
class OrderMap {
   public:
    static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};

    explicit OrderMap(std::size_t expected = 1024) { rehash(capacity_for(expected)); }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    void reserve(std::size_t expected) {
        const std::size_t want = capacity_for(expected);
        if (want > slots_.size()) rehash(want);
    }

    V* find(std::uint64_t key) noexcept {
        const std::size_t i = locate(key);
        return i == kNotFound ? nullptr : &slots_[i].val;
    }

    // Inserts key -> val. Returns {pointer to value, inserted}. If the key is
    // already present the existing value is left unchanged.
    std::pair<V*, bool> insert(std::uint64_t key, const V& val) {
        if (const std::size_t at = locate(key); at != kNotFound) return {&slots_[at].val, false};
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        // Robin Hood: walk forward; whenever the resident entry is closer to its
        // home than the entry we carry, swap them and carry the resident on.
        Slot carry{key, val};
        std::size_t i = home(key), d = 0, placed = kNotFound;
        while (true) {
            Slot& s = slots_[i];
            if (s.key == kEmpty) {
                s = carry;
                ++size_;
                if (placed == kNotFound) placed = i;
                return {&slots_[placed].val, true};
            }
            const std::size_t sd = dist(i, s.key);
            if (sd < d) {
                std::swap(s, carry);
                if (placed == kNotFound) placed = i;
                d = sd;
            }
            i = (i + 1) & mask_;
            ++d;
        }
    }

    bool erase(std::uint64_t key) noexcept {
        std::size_t i = locate(key);
        if (i == kNotFound) return false;
        // Backward shift: pull following entries back one slot until an empty slot
        // or an entry already at its home. With Robin Hood ordering nothing beyond
        // that point can belong before it, so deletion is O(local run), not
        // O(cluster): sequential references form one long cluster of entries that
        // all sit at home, and deleting from it stops immediately.
        while (true) {
            const std::size_t j = (i + 1) & mask_;
            const Slot& n = slots_[j];
            if (n.key == kEmpty || dist(j, n.key) == 0) break;
            slots_[i] = n;
            i = j;
        }
        slots_[i].key = kEmpty;
        --size_;
        return true;
    }

    template <class F>
    void for_each(F&& f) const {
        for (const Slot& s : slots_)
            if (s.key != kEmpty) f(s.key, s.val);
    }

   private:
    struct Slot {
        std::uint64_t key = kEmpty;
        V val{};
    };

    static std::size_t capacity_for(std::size_t expected) {
        return std::bit_ceil(expected * 2 < 16 ? std::size_t{16} : expected * 2);
    }

    static constexpr std::size_t kNotFound = ~std::size_t{0};

    // Probe distance of the entry stored at slot i.
    std::size_t dist(std::size_t i, std::uint64_t key) const noexcept {
        return (i - home(key)) & mask_;
    }

    std::size_t locate(std::uint64_t key) const noexcept {
        std::size_t i = home(key), d = 0;
        while (true) {
            const Slot& s = slots_[i];
            if (s.key == key) return i;
            // Robin Hood invariant: once residents are closer to home than we
            // would be, the key cannot be further along.
            if (s.key == kEmpty || dist(i, s.key) < d) return kNotFound;
            i = (i + 1) & mask_;
            ++d;
        }
    }

    std::size_t home(std::uint64_t key) const noexcept {
        return Hash::slot(key, mask_, shift_);
    }

    void rehash(std::size_t cap) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        shift_ = 64 - std::countr_zero(cap);
        size_ = 0;
        for (const Slot& s : old)
            if (s.key != kEmpty) insert(s.key, s.val);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    int shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace lob
