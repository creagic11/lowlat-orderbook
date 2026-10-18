// itch_stats: decode an ITCH 5.0 file, count messages by type, measure decode speed.
//
//   itch_stats FILE [--passes N]
//
// The first pass also validates framing and message lengths. Later passes run on
// the page-cached mapping, so the timing measures the decoder, not the disk.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lob/itch.hpp"
#include "lob/itch_file.hpp"

using namespace lob::itch;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE [--passes N]\n", argv[0]);
        return 2;
    }
    int passes = 3;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--passes") == 0) passes = std::atoi(argv[i + 1]);

    MappedFile file(argv[1]);
    std::array<std::size_t, 256> by_type{};
    std::size_t book_events = 0, unknown = 0, bad_length = 0;
    std::uint64_t checksum = 0;  // keeps the decode from being optimised away
    FramingResult framing{};
    double best_sec = 1e300;

    for (int pass = 0; pass < passes; ++pass) {
        const bool count = (pass == 0);
        auto t0 = std::chrono::steady_clock::now();
        framing = for_each_message(file.data(), file.size(),
                                   [&](const std::uint8_t* m, std::size_t len) {
                                       Event ev;
                                       const Decode d = decode(m, len, ev);
                                       checksum += ev.ref ^ ev.price ^ ev.shares;
                                       if (!count) return;
                                       ++by_type[m[0]];
                                       if (d == Decode::BookEvent) ++book_events;
                                       else if (d == Decode::Unknown) ++unknown;
                                       else if (d == Decode::BadLength) ++bad_length;
                                   });
        const double sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (pass > 0 || passes == 1) best_sec = sec < best_sec ? sec : best_sec;
    }

    std::printf("file          %s\n", argv[1]);
    std::printf("bytes         %zu\n", file.size());
    std::printf("messages      %zu%s\n", framing.messages, framing.truncated ? "  (TRUNCATED)" : "");
    std::printf("book events   %zu\n", book_events);
    std::printf("unknown type  %zu\n", unknown);
    std::printf("bad length    %zu\n", bad_length);
    std::printf("by type      ");
    for (int t = 0; t < 256; ++t)
        if (by_type[t]) std::printf(" %c=%zu", static_cast<char>(t), by_type[t]);
    std::printf("\n");
    std::printf("decode best   %.3f s over %d pass(es): %.1f M msgs/s, %.2f GB/s  [chk %llu]\n",
                best_sec, passes, static_cast<double>(framing.messages) / best_sec / 1e6,
                static_cast<double>(framing.bytes_consumed) / best_sec / 1e9,
                static_cast<unsigned long long>(checksum & 0xff));
    return (unknown || bad_length || framing.truncated) ? 1 : 0;
}
