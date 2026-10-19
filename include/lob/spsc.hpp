#pragma once
// Bounded single-producer single-consumer ring, lock-free.
//
// One thread pushes, one thread pops. The producer owns `tail_`, the consumer
// owns `head_`; each sits on its own cache line so the two cores never write
// the same line (no false sharing). Each side also keeps a private copy of the
// other side's index and re-reads the shared one only when the ring looks full
// (producer) or empty (consumer): in steady state a push or pop touches no
// cache line the other core is writing.
//
// Ordering: the producer writes the slot, then publishes tail_ with release;
// the consumer reads tail_ with acquire before reading the slot (and the
// mirror image for head_), so a popped slot is always fully written.
//
// Capacity N must be a power of two; indices run freely and are masked.
// The two template switches exist for the ablation benchmarks only.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace lob {

inline constexpr std::size_t kCacheLine = 64;

template <class T, std::size_t N, bool CacheIndices = true, bool Pad = true>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "slots are copied, not constructed");

    static constexpr std::size_t kAlign = Pad ? kCacheLine : alignof(std::uint64_t);

   public:
    static constexpr std::size_t capacity() { return N; }

    // Producer side ------------------------------------------------------
    bool try_push(const T& v) noexcept {
        const std::uint64_t t = prod_.tail.load(std::memory_order_relaxed);
        if (t - head_seen() >= N) return false;           // full
        slots_[t & (N - 1)] = v;
        prod_.tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer side ------------------------------------------------------
    bool try_pop(T& out) noexcept {
        const std::uint64_t h = cons_.head.load(std::memory_order_relaxed);
        if (h == tail_seen(h)) return false;              // empty
        out = slots_[h & (N - 1)];
        cons_.head.store(h + 1, std::memory_order_release);
        return true;
    }

    // Pops up to max items into out[]; one index publish for the whole batch.
    std::size_t pop_batch(T* out, std::size_t max) noexcept {
        const std::uint64_t h = cons_.head.load(std::memory_order_relaxed);
        std::uint64_t avail = tail_seen(h) - h;
        if constexpr (CacheIndices) {
            if (avail < max) {                                // cached tail may be stale: refresh once
                cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);
                avail = cons_.tail_cache - h;
            }
        }
        if (avail == 0) return 0;
        if (avail > max) avail = max;
        for (std::uint64_t i = 0; i < avail; ++i) out[i] = slots_[(h + i) & (N - 1)];
        cons_.head.store(h + avail, std::memory_order_release);
        return static_cast<std::size_t>(avail);
    }

    // Approximate (exact when called from either thread while the other is idle).
    std::size_t size() const noexcept {
        return static_cast<std::size_t>(prod_.tail.load(std::memory_order_acquire) -
                                        cons_.head.load(std::memory_order_acquire));
    }

   private:
    // Producer's view of head: cached, refreshed only when the ring looks full.
    std::uint64_t head_seen() noexcept {
        if constexpr (CacheIndices) {
            const std::uint64_t t = prod_.tail.load(std::memory_order_relaxed);
            if (t - prod_.head_cache >= N)
                prod_.head_cache = cons_.head.load(std::memory_order_acquire);
            return prod_.head_cache;
        } else {
            return cons_.head.load(std::memory_order_acquire);
        }
    }
    // Consumer's view of tail: cached, refreshed only when the ring looks empty.
    std::uint64_t tail_seen(std::uint64_t h) noexcept {
        if constexpr (CacheIndices) {
            if (h == cons_.tail_cache) cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);
            return cons_.tail_cache;
        } else {
            return prod_.tail.load(std::memory_order_acquire);
        }
    }

    struct alignas(kAlign) Producer {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t head_cache{0};
    };
    struct alignas(kAlign) Consumer {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t tail_cache{0};
    };

    Producer prod_;
    Consumer cons_;
    alignas(kAlign) T slots_[N];
};

}  // namespace lob
