// feed: UDP multicast MoldUDP64 feed handler driving the order book.
//
//   feed [--group 239.1.1.1] [--port 26477] [--iface 127.0.0.1] [--rcvbuf BYTES]
//        [--idle-ms 3000] [--bbo-out OUT.bbo] [--compare REF.bbo]
//
// Joins the group, receives with recvmmsg in batches, and for each packet checks
// the sequence number: a packet starting beyond the expected number is a gap
// (the missing count is reported; a production handler would re-request it from
// the MoldUDP64 re-request server or recover from a snapshot), and messages below
// it are duplicates and skipped. Messages are decoded and applied to FastBook.
// Stops at end of session, or after --idle-ms without data.
//
// BBO records carry seq = MoldUDP64 sequence - 1, i.e. the message's index in the
// original file, so with no loss the stream is identical to `replay --bbo-out`.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_builder.hpp"
#include "lob/itch.hpp"
#include "lob/itch_file.hpp"
#include "lob/moldudp64.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    std::string group = "239.1.1.1", iface = "127.0.0.1", bbo_out, compare;
    int port = 26477, idle_ms = 3000, rcvbuf = 64 << 20;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--group") group = next();
        else if (a == "--iface") iface = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--rcvbuf") rcvbuf = std::atoi(next().c_str());
        else if (a == "--idle-ms") idle_ms = std::atoi(next().c_str());
        else if (a == "--bbo-out") bbo_out = next();
        else if (a == "--compare") compare = next();
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    int got_buf = 0;
    socklen_t gl = sizeof got_buf;
    ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got_buf, &gl);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<std::uint16_t>(port));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        std::perror("bind");
        return 1;
    }
    ip_mreq mreq{};
    ::inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
    ::inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface);
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
        std::perror("IP_ADD_MEMBERSHIP");
        return 1;
    }
    timeval tv{idle_ms / 1000, (idle_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::fprintf(stderr, "feed: joined %s:%d on %s, SO_RCVBUF %d bytes\n", group.c_str(), port,
                 iface.c_str(), got_buf);

    std::FILE* out = bbo_out.empty() ? nullptr : std::fopen(bbo_out.c_str(), "wb");
    std::unique_ptr<itch::MappedFile> ref_file;
    const BboRecord* ref = nullptr;
    std::size_t ref_n = 0, records = 0, first_mismatch = ~std::size_t{0};
    if (!compare.empty()) {
        ref_file = std::make_unique<itch::MappedFile>(compare);
        ref = reinterpret_cast<const BboRecord*>(ref_file->data());
        ref_n = ref_file->size() / sizeof(BboRecord);
    }

    auto builder = std::make_unique<BookBuilder<FastBook>>(std::size_t{1} << 22);
    auto on_bbo = [&](const BboRecord& r) {
        if (out) std::fwrite(&r, sizeof r, 1, out);
        if (ref && first_mismatch == ~std::size_t{0} &&
            (records >= ref_n || std::memcmp(&r, &ref[records], sizeof r) != 0))
            first_mismatch = records;
        ++records;
    };

    constexpr int kBatch = 64, kMaxPkt = 2048;
    static std::uint8_t bufs[kBatch][kMaxPkt];
    mmsghdr msgs[kBatch];
    iovec iov[kBatch];
    for (int i = 0; i < kBatch; ++i) {
        iov[i] = {bufs[i], kMaxPkt};
        msgs[i] = {};
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    std::uint64_t expected = 0, packets = 0, messages = 0, gaps = 0, missing = 0, dups = 0,
                  heartbeats = 0, malformed = 0, syscalls = 0;
    bool synced = false, eos = false;
    Clock::time_point first_pkt{}, last_pkt{};

    while (!eos) {
        const int n = ::recvmmsg(fd, msgs, kBatch, MSG_WAITFORONE, nullptr);
        if (n <= 0) break;  // idle timeout: the sender is gone
        ++syscalls;
        last_pkt = Clock::now();
        if (packets == 0) first_pkt = last_pkt;
        for (int k = 0; k < n && !eos; ++k) {
            const std::uint8_t* p = bufs[k];
            const std::size_t len = msgs[k].msg_len;
            mold::Header h;
            if (!mold::parse_header(p, len, h)) {
                ++malformed;
                continue;
            }
            ++packets;
            if (h.count == mold::kEndOfSession) {
                eos = true;
                break;
            }
            if (synced && h.seq > expected) {
                ++gaps;
                missing += h.seq - expected;
            }
            if (h.count == 0) ++heartbeats;
            const std::uint64_t end = h.seq + h.count;
            const std::uint64_t from = synced && expected > h.seq ? expected : h.seq;
            if (!synced || end > expected) expected = end;
            synced = true;
            const bool ok = mold::for_each_message(p, len, h, [&](std::uint64_t seq, const std::uint8_t* m,
                                                                  std::size_t ml) {
                if (seq < from) {
                    ++dups;
                    return;
                }
                ++messages;
                itch::Event ev;
                if (itch::decode(m, ml, ev) == itch::Decode::BookEvent)
                    builder->on_event(seq - 1, ev, on_bbo);
            });
            if (!ok) ++malformed;
        }
    }
    if (out) std::fclose(out);
    const double sec = std::chrono::duration<double>(last_pkt - first_pkt).count();

    std::printf("packets       %llu (%llu heartbeats, %llu malformed), end of session: %s\n",
                (unsigned long long)packets, (unsigned long long)heartbeats,
                (unsigned long long)malformed, eos ? "yes" : "NO (idle timeout)");
    std::printf("messages      %llu applied, %llu duplicates skipped\n", (unsigned long long)messages,
                (unsigned long long)dups);
    std::printf("gaps          %llu (%llu messages missing)\n", (unsigned long long)gaps,
                (unsigned long long)missing);
    std::printf("book          %llu BBO updates, %zu live orders, %llu unknown refs\n",
                (unsigned long long)builder->bbo_updates(), builder->book().live_orders(),
                (unsigned long long)builder->book().counters().unknown_ref);
    std::printf("receive       %.3f s, %.2f M msgs/s, %.1f packets per recvmmsg\n", sec,
                sec > 0 ? static_cast<double>(messages) / sec / 1e6 : 0.0,
                syscalls ? static_cast<double>(packets) / static_cast<double>(syscalls) : 0.0);
    if (ref) {
        if (first_mismatch == ~std::size_t{0} && records == ref_n) {
            std::printf("compare       %zu BBO records identical to %s\n", records, compare.c_str());
        } else {
            std::printf("MISMATCH: first difference at record %zu (%zu produced, %zu in reference)\n",
                        first_mismatch, records, ref_n);
            return 1;
        }
    }
    ::close(fd);
    return eos ? 0 : 3;
}
