#pragma once
// Latency measurement: a cheap clock and a fixed-memory histogram.
//
// TscClock reads the CPU timestamp counter (rdtsc, ~20 cycles, no system call)
// and converts ticks to nanoseconds with a factor calibrated against
// std::chrono::steady_clock at start-up. Modern x86 TSCs run at a constant rate
// on every core (constant_tsc / nonstop_tsc), so stamps taken on one core and
// compared on another are valid. Elsewhere it falls back to steady_clock.
//
// LatencyHistogram records nanosecond values without allocating: exact 1 ns
// buckets below 16,384 ns, then 128 sub-buckets per power of two (under 1%
// error) up to ~2^40 ns. Percentiles return the bucket's upper bound.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define LOB_HAVE_TSC 1
#else
#define LOB_HAVE_TSC 0
#endif

namespace lob {

inline std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

class TscClock {
   public:
    // Calibrates over `ms` milliseconds (blocks the caller).
    explicit TscClock(int ms = 50) {
#if LOB_HAVE_TSC
        const std::uint64_t t0 = steady_ns(), c0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        const std::uint64_t t1 = steady_ns(), c1 = __rdtsc();
        ns_per_tick_ = static_cast<double>(t1 - t0) / static_cast<double>(c1 - c0);
#else
        (void)ms;
#endif
    }
    std::uint64_t now() const {
#if LOB_HAVE_TSC
        return __rdtsc();
#else
        return steady_ns();
#endif
    }
    std::uint64_t to_ns(std::uint64_t ticks) const {
        return static_cast<std::uint64_t>(static_cast<double>(ticks) * ns_per_tick_);
    }
    double ghz() const { return 1.0 / ns_per_tick_; }

   private:
    double ns_per_tick_ = 1.0;
};

class LatencyHistogram {
    static constexpr unsigned kLinearBits = 14;                 // 0..16383 ns exact
    static constexpr std::uint64_t kLinear = 1ull << kLinearBits;
    static constexpr unsigned kSubBits = 7;                     // 128 sub-buckets per octave
    static constexpr unsigned kOctaves = 40 - kLinearBits;      // up to 2^40 ns (~18 min)
    static constexpr std::size_t kBuckets = kLinear + (std::size_t(kOctaves) << kSubBits);

   public:
    void record(std::uint64_t ns) {
        ++counts_[index(ns)];
        ++n_;
        sum_ += ns;
        max_ = std::max(max_, ns);
        min_ = std::min(min_, ns);
    }
    std::uint64_t count() const { return n_; }
    std::uint64_t max() const { return n_ ? max_ : 0; }
    std::uint64_t min() const { return n_ ? min_ : 0; }
    double mean() const { return n_ ? static_cast<double>(sum_) / static_cast<double>(n_) : 0.0; }

    // Smallest bucket upper bound with at least p (0..1) of the samples at or below it.
    std::uint64_t percentile(double p) const {
        if (n_ == 0) return 0;
        const std::uint64_t want = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(p * static_cast<double>(n_) + 0.5));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= want) return std::min(upper(i), max_);
        }
        return max_;
    }

    void merge(const LatencyHistogram& o) {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        n_ += o.n_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

   private:
    static std::size_t index(std::uint64_t ns) {
        if (ns < kLinear) return static_cast<std::size_t>(ns);
        const unsigned msb = 63u - static_cast<unsigned>(__builtin_clzll(ns));   // >= kLinearBits
        const unsigned oct = std::min(msb - kLinearBits, kOctaves - 1);
        const std::uint64_t sub = (ns >> (msb - kSubBits)) & ((1u << kSubBits) - 1);
        return static_cast<std::size_t>(kLinear + (std::size_t(oct) << kSubBits) + sub);
    }
    static std::uint64_t upper(std::size_t i) {
        if (i < kLinear) return i;
        const std::size_t j = i - kLinear;
        const unsigned msb = static_cast<unsigned>(j >> kSubBits) + kLinearBits;
        const std::uint64_t sub = j & ((1u << kSubBits) - 1);
        return ((1ull << msb) | ((sub + 1) << (msb - kSubBits))) - 1;
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t n_ = 0, sum_ = 0, max_ = 0, min_ = ~0ull;
};

}  // namespace lob
