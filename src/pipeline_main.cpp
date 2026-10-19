// pipeline: the replay split across two threads joined by a lock-free SPSC ring.
//
//   pipeline FILE [--threads 1|2] [--cpu A,B] [--rate MSGS_PER_S] [--batch N]
//                 [--ring default|nocache|nopad] [--bbo-out OUT] [--compare REF]
//                 [--locate N] [--max-msgs N]
//
//   feed thread (core A): walk the mapped file, decode, stamp the TSC, push
//                         {seq, t_in, event} into the ring (spin while full)
//   book thread (core B): pop in batches, apply to the FastBook, emit BBO changes
//
// Latency is measured per book event from the feed thread's stamp (taken right
// after decoding, i.e. when the event is ready) to the moment the book thread has
// applied it and, for BBO changes, has the new BBO: decode-to-BBO through the ring.
// --rate paces the feed (unloaded latency, no queueing); without it the feed runs
// flat out (throughput; latency then includes time spent queued in the ring).
// --threads 1 runs the same loop in one thread (no ring) as the baseline.
// --ring selects ablation variants of the ring (no cached indices / no padding).

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_builder.hpp"
#include "lob/itch.hpp"
#include "lob/itch_file.hpp"
#include "lob/latency.hpp"
#include "lob/spsc.hpp"

using namespace lob;

namespace {

struct Options {
    std::string file, bbo_out, compare, ring = "default";
    int threads = 2, cpu_a = -1, cpu_b = -1, locate = -1;
    std::size_t batch = 64, max_msgs = ~std::size_t{0};
    double rate = 0;  // msgs/s, 0 = unpaced
};

struct Item {
    std::uint64_t seq;
    std::uint64_t t_in;  // TSC ticks when the event was decoded
    itch::Event ev;
};
constexpr std::uint64_t kEnd = ~0ull;
constexpr std::size_t kRing = 4096;

void pin(int cpu) {
    if (cpu < 0) return;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof s, &s) != 0)
        std::fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
}

struct Sink {  // BBO stream: written to a file and/or compared against a reference
    const BboRecord* ref = nullptr;
    std::size_t ref_n = 0, records = 0, first_mismatch = ~std::size_t{0};
    std::FILE* out = nullptr;
    int locate = -1;
    void operator()(const BboRecord& rec) {
        if (locate >= 0 && rec.locate != locate) return;
        if (out) std::fwrite(&rec, sizeof rec, 1, out);
        if (ref && first_mismatch == ~std::size_t{0} &&
            (records >= ref_n || std::memcmp(&rec, &ref[records], sizeof rec) != 0))
            first_mismatch = records;
        ++records;
    }
};

struct Stats {
    std::size_t messages = 0, events = 0;
    double seconds = 0;
    std::unique_ptr<LatencyHistogram> all = std::make_unique<LatencyHistogram>();  // every book event
    std::unique_ptr<LatencyHistogram> bbo = std::make_unique<LatencyHistogram>();  // events that moved a BBO
};

// Feed side: calls push(item) for every book event (paced if asked), returns messages read.
template <class Push>
std::size_t feed(const itch::MappedFile& f, const Options& o, const TscClock& clk, Push&& push) {
    const double ticks_per_msg = o.rate > 0 ? clk.ghz() * 1e9 / o.rate : 0;
    const std::uint64_t start = clk.now();
    std::uint64_t seq = 0, n_ev = 0;
    std::size_t msgs = 0;
    itch::for_each_message(f.data(), f.size(), [&](const std::uint8_t* m, std::size_t len) {
        if (msgs >= o.max_msgs) return;
        ++msgs;
        Item it;
        if (itch::decode(m, len, it.ev) == itch::Decode::BookEvent) {
            if (ticks_per_msg > 0) {
                const auto due = start + static_cast<std::uint64_t>(static_cast<double>(n_ev) * ticks_per_msg);
                while (clk.now() < due) {}
            }
            it.seq = seq;
            it.t_in = clk.now();
            push(it);
            ++n_ev;
        }
        ++seq;
    });
    return msgs;
}

template <class Builder>
inline void apply(Builder& b, const Item& it, const TscClock& clk, Stats& st, Sink& sink) {
    bool moved = false;
    b.on_event(it.seq, it.ev, [&](const BboRecord& rec) {
        moved = true;
        sink(rec);
    });
    const std::uint64_t ns = clk.to_ns(clk.now() - it.t_in);
    st.all->record(ns);
    if (moved) st.bbo->record(ns);
    ++st.events;
}

Stats run_single(const itch::MappedFile& f, const Options& o, const TscClock& clk, Sink& sink) {
    Stats st;
    pin(o.cpu_a);
    auto b = std::make_unique<BookBuilder<FastBook>>(1 << 22);
    const auto t0 = steady_ns();
    st.messages = feed(f, o, clk, [&](const Item& it) { apply(*b, it, clk, st, sink); });
    st.seconds = static_cast<double>(steady_ns() - t0) / 1e9;
    return st;
}

template <class Ring>
Stats run_pipeline(const itch::MappedFile& f, const Options& o, const TscClock& clk, Sink& sink) {
    Stats st;
    auto ring = std::make_unique<Ring>();
    auto b = std::make_unique<BookBuilder<FastBook>>(1 << 22);
    std::atomic<bool> go{false};
    std::size_t msgs = 0;

    std::thread producer([&] {
        pin(o.cpu_a);
        while (!go.load(std::memory_order_acquire)) {}
        msgs = feed(f, o, clk, [&](const Item& it) {
            while (!ring->try_push(it)) {}
        });
        Item end{};
        end.seq = kEnd;
        while (!ring->try_push(end)) {}
    });

    pin(o.cpu_b);
    std::vector<Item> buf(o.batch);
    const auto t0 = steady_ns();
    go.store(true, std::memory_order_release);
    for (bool done = false; !done;) {
        const std::size_t k = o.batch == 1 ? (ring->try_pop(buf[0]) ? 1 : 0)
                                           : ring->pop_batch(buf.data(), o.batch);
        for (std::size_t i = 0; i < k; ++i) {
            if (buf[i].seq == kEnd) { done = true; break; }
            apply(*b, buf[i], clk, st, sink);
        }
    }
    st.seconds = static_cast<double>(steady_ns() - t0) / 1e9;
    producer.join();
    st.messages = msgs;
    return st;
}

void print_hist(const char* name, const LatencyHistogram& h) {
    std::printf("%-10s n=%-10llu p50 %6llu  p90 %6llu  p99 %6llu  p99.9 %7llu  p99.99 %8llu  max %9llu ns\n", name,
                (unsigned long long)h.count(), (unsigned long long)h.percentile(0.50),
                (unsigned long long)h.percentile(0.90), (unsigned long long)h.percentile(0.99),
                (unsigned long long)h.percentile(0.999), (unsigned long long)h.percentile(0.9999),
                (unsigned long long)h.max());
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--threads") o.threads = std::atoi(next().c_str());
        else if (a == "--cpu") {
            const std::string v = next();
            o.cpu_a = std::atoi(v.c_str());
            const auto comma = v.find(',');
            o.cpu_b = comma == std::string::npos ? -1 : std::atoi(v.c_str() + comma + 1);
        }
        else if (a == "--rate") o.rate = std::atof(next().c_str());
        else if (a == "--batch") o.batch = std::max<std::size_t>(1, std::strtoull(next().c_str(), nullptr, 10));
        else if (a == "--ring") o.ring = next();
        else if (a == "--bbo-out") o.bbo_out = next();
        else if (a == "--compare") o.compare = next();
        else if (a == "--locate") o.locate = std::atoi(next().c_str());
        else if (a == "--max-msgs") o.max_msgs = std::strtoull(next().c_str(), nullptr, 10);
        else if (o.file.empty()) o.file = a;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (o.file.empty() || (o.threads != 1 && o.threads != 2) ||
        (o.ring != "default" && o.ring != "nocache" && o.ring != "nopad")) {
        std::fprintf(stderr, "usage: %s FILE [--threads 1|2] [--cpu A,B] [--rate N] [--batch N]"
                     " [--ring default|nocache|nopad] [--bbo-out OUT] [--compare REF] [--locate N]"
                     " [--max-msgs N]\n", argv[0]);
        return 2;
    }

    itch::MappedFile f(o.file);
    std::unique_ptr<itch::MappedFile> ref_file;
    Sink sink;
    sink.locate = o.locate;
    if (!o.compare.empty()) {
        ref_file = std::make_unique<itch::MappedFile>(o.compare);
        sink.ref = reinterpret_cast<const BboRecord*>(ref_file->data());
        sink.ref_n = ref_file->size() / sizeof(BboRecord);
    }
    if (!o.bbo_out.empty() && !(sink.out = std::fopen(o.bbo_out.c_str(), "wb"))) {
        std::fprintf(stderr, "cannot write %s\n", o.bbo_out.c_str());
        return 1;
    }

    const TscClock clk(100);
    Stats st;
    if (o.threads == 1) st = run_single(f, o, clk, sink);
    else if (o.ring == "nocache") st = run_pipeline<SpscRing<Item, kRing, false, true>>(f, o, clk, sink);
    else if (o.ring == "nopad") st = run_pipeline<SpscRing<Item, kRing, true, false>>(f, o, clk, sink);
    else st = run_pipeline<SpscRing<Item, kRing>>(f, o, clk, sink);
    if (sink.out) std::fclose(sink.out);

    std::printf("mode          %s, ring %s, batch %zu, cpus %d,%d, rate %s\n",
                o.threads == 1 ? "1 thread" : "2 threads", o.ring.c_str(), o.batch, o.cpu_a, o.cpu_b,
                o.rate > 0 ? (std::to_string(static_cast<long long>(o.rate)) + " msgs/s").c_str() : "unpaced");
    std::printf("tsc           %.3f GHz\n", clk.ghz());
    std::printf("messages      %zu (%zu book events)\n", st.messages, st.events);
    std::printf("time          %.3f s: %.2f M msgs/s, %.1f ns/msg\n", st.seconds,
                static_cast<double>(st.messages) / st.seconds / 1e6,
                st.seconds * 1e9 / static_cast<double>(st.messages));
    print_hist("all events", *st.all);
    print_hist("bbo moves", *st.bbo);
    if (sink.out) std::printf("wrote         %zu BBO records to %s\n", sink.records, o.bbo_out.c_str());
    if (sink.ref) {
        if (sink.first_mismatch != ~std::size_t{0} || sink.records != sink.ref_n) {
            std::printf("MISMATCH: first at record %zu; produced %zu, reference %zu\n",
                        sink.first_mismatch, sink.records, sink.ref_n);
            return 1;
        }
        std::printf("compare       OK: %zu BBO records identical to %s\n", sink.records, o.compare.c_str());
    }
    return 0;
}
