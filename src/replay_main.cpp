// replay: run an ITCH 5.0 file through an order book, single-threaded.
//
//   replay FILE [--book fast|baseline] [--bbo-out OUT.bbo] [--compare REF.bbo]
//               [--locate N] [--passes N] [--expect-orders N]
//
// --bbo-out   writes the BBO-change stream (same format as tools/ref_book.py)
// --compare   checks the stream against a reference stream, record by record
// --passes    repeats the replay (fresh book each time) and reports the best time
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
    std::vector<BboRecord> stream;
};

template <class Book>
Result run(const itch::MappedFile& f, const Options& o, bool keep_stream) {
    Result r;
    auto builder = std::make_unique<BookBuilder<Book>>(o.expect_orders);
    if (keep_stream) r.stream.reserve(1 << 20);
    std::uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto fr = itch::for_each_message(f.data(), f.size(), [&](const std::uint8_t* m, std::size_t len) {
        itch::Event ev;
        if (itch::decode(m, len, ev) == itch::Decode::BookEvent) {
            builder->on_event(seq, ev, [&](const BboRecord& rec) {
                if (keep_stream && (o.locate < 0 || rec.locate == o.locate)) r.stream.push_back(rec);
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

std::vector<BboRecord> read_stream(const std::string& path) {
    itch::MappedFile f(path);
    std::vector<BboRecord> v(f.size() / sizeof(BboRecord));
    if (!v.empty()) std::memcpy(v.data(), f.data(), v.size() * sizeof(BboRecord));
    return v;
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
    const bool keep = !o.bbo_out.empty() || !o.compare.empty();
    Result best;
    best.seconds = 1e300;
    for (int p = 0; p < o.passes; ++p) {
        Result r = (o.book == "fast") ? run<FastBook>(f, o, keep && p == 0)
                                      : run<BaselineBook>(f, o, keep && p == 0);
        if (p == 0) best.stream = std::move(r.stream);
        if (r.seconds < best.seconds) {
            auto s = std::move(best.stream);
            best = std::move(r);
            best.stream = std::move(s);
        }
    }

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

    if (!o.bbo_out.empty()) {
        std::FILE* out = std::fopen(o.bbo_out.c_str(), "wb");
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", o.bbo_out.c_str());
            return 1;
        }
        std::fwrite(best.stream.data(), sizeof(BboRecord), best.stream.size(), out);
        std::fclose(out);
        std::printf("wrote         %zu BBO records to %s\n", best.stream.size(), o.bbo_out.c_str());
    }
    if (!o.compare.empty()) {
        const auto ref = read_stream(o.compare);
        std::size_t n = ref.size() < best.stream.size() ? ref.size() : best.stream.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (std::memcmp(&ref[i], &best.stream[i], sizeof(BboRecord)) != 0) {
                const auto& a = ref[i];
                const auto& b = best.stream[i];
                std::printf("MISMATCH at record %zu: ref seq=%llu loc=%u bid=%u x %llu ask=%u x %llu"
                            " | got seq=%llu loc=%u bid=%u x %llu ask=%u x %llu\n", i,
                            (unsigned long long)a.seq, a.locate, a.bid_px, (unsigned long long)a.bid_qty,
                            a.ask_px, (unsigned long long)a.ask_qty, (unsigned long long)b.seq,
                            b.locate, b.bid_px, (unsigned long long)b.bid_qty, b.ask_px,
                            (unsigned long long)b.ask_qty);
                return 1;
            }
        }
        if (ref.size() != best.stream.size()) {
            std::printf("MISMATCH: reference has %zu records, book produced %zu\n", ref.size(),
                        best.stream.size());
            return 1;
        }
        std::printf("compare       %zu BBO records identical to %s\n", n, o.compare.c_str());
    }
    return 0;
}
