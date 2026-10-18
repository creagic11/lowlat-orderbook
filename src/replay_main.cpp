// replay: run an ITCH 5.0 file through an order book, single-threaded.
//
//   replay FILE [--book fast|baseline] [--bbo-out OUT.bbo] [--compare REF.bbo]
//               [--locate N] [--passes N] [--expect-orders N]
//
// --bbo-out   writes the BBO-change stream (same format as tools/ref_book.py)
// --compare   checks the stream against a reference stream, record by record
// --passes    repeats the replay (fresh book each time) and reports the best time;
//             with --compare/--bbo-out, pass 1 does the checking and later passes
//             are timed without it
//
// Timing covers decode + book update + BBO change detection for every message,
// over a file already mapped into memory.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_baseline.hpp"
#include "lob/book_builder.hpp"
#include "lob/itch.hpp"
#include "lob/itch_file.hpp"

using namespace lob;

namespace {

struct Options {
    std::string file, book = "fast", bbo_out, compare;
    int locate = -1, passes = 1;
    std::size_t expect_orders = 1 << 22;
};

struct Result {
    double seconds = 0;
    std::size_t messages = 0;
    std::uint64_t bbo_updates = 0;
    std::size_t live_orders = 0;
    BookCounters counters;
    std::size_t records = 0;          // BBO records produced (after --locate filter)
    std::size_t first_mismatch = ~std::size_t{0};
    BboRecord got_at_mismatch{};
};

// Where the first pass sends its BBO stream: compared on the fly against a mapped
// reference and/or written to a file. Nothing is held in memory, so a full trading
// day (tens of millions of records) needs no RAM beyond the books themselves.
struct StreamSink {
    const BboRecord* ref = nullptr;
    std::size_t ref_n = 0;
    std::FILE* out = nullptr;
};

template <class Book>
Result run(const itch::MappedFile& f, const Options& o, const StreamSink* sink) {
    Result r;
    auto builder = std::make_unique<BookBuilder<Book>>(o.expect_orders);
    std::uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto fr = itch::for_each_message(f.data(), f.size(), [&](const std::uint8_t* m, std::size_t len) {
        itch::Event ev;
        if (itch::decode(m, len, ev) == itch::Decode::BookEvent) {
            builder->on_event(seq, ev, [&](const BboRecord& rec) {
                if (!sink || (o.locate >= 0 && rec.locate != o.locate)) return;
                if (sink->out) std::fwrite(&rec, sizeof rec, 1, sink->out);
                if (sink->ref && r.first_mismatch == ~std::size_t{0} &&
                    (r.records >= sink->ref_n ||
                     std::memcmp(&rec, &sink->ref[r.records], sizeof rec) != 0)) {
                    r.first_mismatch = r.records;
                    r.got_at_mismatch = rec;
                }
                ++r.records;
            });
        }
        ++seq;
    });
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.messages = fr.messages;
    r.bbo_updates = builder->bbo_updates();
    r.live_orders = builder->book().live_orders();
    r.counters = builder->book().counters();
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--book") o.book = next();
        else if (a == "--bbo-out") o.bbo_out = next();
        else if (a == "--compare") o.compare = next();
        else if (a == "--locate") o.locate = std::atoi(next().c_str());
        else if (a == "--passes") o.passes = std::atoi(next().c_str());
        else if (a == "--expect-orders") o.expect_orders = std::strtoull(next().c_str(), nullptr, 10);
        else if (o.file.empty()) o.file = a;
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (o.file.empty() || (o.book != "fast" && o.book != "baseline")) {
        std::fprintf(stderr,
                     "usage: %s FILE [--book fast|baseline] [--bbo-out OUT] [--compare REF]"
                     " [--locate N] [--passes N]\n", argv[0]);
        return 2;
    }

    itch::MappedFile f(o.file);
    std::unique_ptr<itch::MappedFile> ref_file;
    StreamSink sink;
    if (!o.compare.empty()) {
        ref_file = std::make_unique<itch::MappedFile>(o.compare);
        sink.ref = reinterpret_cast<const BboRecord*>(ref_file->data());
        sink.ref_n = ref_file->size() / sizeof(BboRecord);
    }
    if (!o.bbo_out.empty()) {
        sink.out = std::fopen(o.bbo_out.c_str(), "wb");
        if (!sink.out) {
            std::fprintf(stderr, "cannot write %s\n", o.bbo_out.c_str());
            return 1;
        }
    }
    const bool use_sink = sink.ref || sink.out;

    Result best, first;
    best.seconds = 1e300;
    for (int p = 0; p < o.passes; ++p) {
        const StreamSink* sp = (use_sink && p == 0) ? &sink : nullptr;
        Result r = (o.book == "fast") ? run<FastBook>(f, o, sp) : run<BaselineBook>(f, o, sp);
        if (p == 0) first = r;
        // With a sink attached, pass 0 also writes/compares; time only clean passes
        // unless there is just one.
        if ((p > 0 || o.passes == 1 || !use_sink) && r.seconds < best.seconds) best = r;
    }
    if (sink.out) std::fclose(sink.out);

    std::printf("book          %s\n", o.book.c_str());
    std::printf("messages      %zu\n", best.messages);
    std::printf("bbo updates   %llu\n", static_cast<unsigned long long>(best.bbo_updates));
    std::printf("live orders   %zu (end of file)\n", best.live_orders);
    std::printf("unknown refs  %llu   duplicate refs %llu   bad side %llu\n",
                static_cast<unsigned long long>(best.counters.unknown_ref),
                static_cast<unsigned long long>(best.counters.duplicate_ref),
                static_cast<unsigned long long>(best.counters.bad_side));
    std::printf("best time     %.3f s over %d pass(es): %.2f M msgs/s, %.1f ns/msg\n", best.seconds,
                o.passes, static_cast<double>(best.messages) / best.seconds / 1e6,
                best.seconds * 1e9 / static_cast<double>(best.messages));
    if (sink.out)
        std::printf("wrote         %zu BBO records to %s\n", first.records, o.bbo_out.c_str());
    if (sink.ref) {
        if (first.first_mismatch != ~std::size_t{0}) {
            const auto i = first.first_mismatch;
            const auto& b = first.got_at_mismatch;
            std::printf("MISMATCH at record %zu: got seq=%llu loc=%u bid=%u x %llu ask=%u x %llu",
                        i, (unsigned long long)b.seq, b.locate, b.bid_px,
                        (unsigned long long)b.bid_qty, b.ask_px, (unsigned long long)b.ask_qty);
            if (i < sink.ref_n) {
                const auto& a = sink.ref[i];
                std::printf(" | ref seq=%llu loc=%u bid=%u x %llu ask=%u x %llu",
                            (unsigned long long)a.seq, a.locate, a.bid_px,
                            (unsigned long long)a.bid_qty, a.ask_px, (unsigned long long)a.ask_qty);
            }
            std::printf("\n");
            return 1;
        }
        if (first.records != sink.ref_n) {
            std::printf("MISMATCH: reference has %zu records, book produced %zu\n", sink.ref_n,
                        first.records);
            return 1;
        }
        std::printf("compare       %zu BBO records identical to %s\n", first.records,
                    o.compare.c_str());
    }
    return 0;
}
