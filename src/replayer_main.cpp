// replayer: turn an ITCH 5.0 file into a MoldUDP64 feed.
//
//   replayer FILE [--send] [--group 239.1.1.1] [--port 26477] [--iface 127.0.0.1]
//                 [--rate MSGS_PER_SEC] [--drop-every N] [--limit N]
//                 [--max-payload 1400] [--pcap OUT.pcap]
//
// Message i of the file (0-based) gets MoldUDP64 sequence number i + 1, so a
// feed handler's BBO stream can be compared with `replay --compare` output.
// --send       multicast the packets (sendmmsg, batches of up to 64)
// --rate       pace to about this many messages per second (default: as fast as possible)
// --drop-every skip sending every Nth packet, to test gap detection
// --pcap       also write the packets that were sent as Ethernet/IPv4/UDP frames
// The feed ends with three end-of-session packets (UDP may lose one).

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
#include <thread>
#include <vector>

#include "lob/endian.hpp"
#include "lob/itch_file.hpp"
#include "lob/moldudp64.hpp"
#include "lob/pcap.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    std::string file, group = "239.1.1.1", iface = "127.0.0.1", pcap_path;
    int port = 26477;
    bool send = false;
    double rate = 0;
    std::size_t drop_every = 0, limit = ~std::size_t{0}, max_payload = 1400;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--send") send = true;
        else if (a == "--group") group = next();
        else if (a == "--iface") iface = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--rate") rate = std::atof(next().c_str());
        else if (a == "--drop-every") drop_every = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--limit") limit = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--max-payload") max_payload = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--pcap") pcap_path = next();
        else if (file.empty()) file = a;
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (file.empty() || (!send && pcap_path.empty())) {
        std::fprintf(stderr, "usage: %s FILE [--send] [--pcap OUT] [--group G] [--port P] [--iface IP]"
                             " [--rate MSGS/S] [--drop-every N] [--limit N] [--max-payload B]\n", argv[0]);
        return 2;
    }

    int fd = -1;
    sockaddr_in dst{};
    if (send) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        in_addr ifa{};
        ::inet_pton(AF_INET, iface.c_str(), &ifa);
        ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
        unsigned char loop = 1, ttl = 1;
        ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
        ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        int sndbuf = 8 << 20;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
        dst.sin_family = AF_INET;
        dst.sin_port = htons(static_cast<std::uint16_t>(port));
        ::inet_pton(AF_INET, group.c_str(), &dst.sin_addr);
    }
    std::unique_ptr<PcapWriter> pcap;
    if (!pcap_path.empty()) {
        in_addr g{}, s{};
        ::inet_pton(AF_INET, group.c_str(), &g);
        ::inet_pton(AF_INET, "10.0.0.1", &s);
        pcap = std::make_unique<PcapWriter>(pcap_path, ntohl(s.s_addr), ntohl(g.s_addr), 40000,
                                            static_cast<std::uint16_t>(port));
    }

    // Batch of finished packets waiting for sendmmsg.
    constexpr int kBatch = 64;
    std::vector<std::vector<std::uint8_t>> batch;
    batch.reserve(kBatch);
    std::size_t packets = 0, sent = 0, dropped_packets = 0, dropped_msgs = 0, send_errors = 0;
    auto flush = [&]() {
        if (!send || batch.empty()) {
            batch.clear();
            return;
        }
        mmsghdr msgs[kBatch];
        iovec iov[kBatch];
        for (std::size_t i = 0; i < batch.size(); ++i) {
            iov[i] = {batch[i].data(), batch[i].size()};
            msgs[i] = {};
            msgs[i].msg_hdr.msg_name = &dst;
            msgs[i].msg_hdr.msg_namelen = sizeof dst;
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }
        std::size_t done = 0;
        while (done < batch.size()) {
            int r = ::sendmmsg(fd, msgs + done, static_cast<unsigned>(batch.size() - done), 0);
            if (r <= 0) {
                ++send_errors;
                std::this_thread::sleep_for(std::chrono::microseconds(50));  // buffer full: back off
                continue;
            }
            done += static_cast<std::size_t>(r);
        }
        sent += batch.size();
        batch.clear();
    };

    static const char kSession[11] = "LOBREPLAY0";
    mold::Packer packer(kSession, 1, max_payload);
    itch::MappedFile f(file);
    std::size_t messages = 0;
    std::uint64_t last_ts = 0;
    const auto t0 = Clock::now();

    auto emit_packet = [&]() {
        if (packer.count() == 0) return;
        const auto& p = packer.finish();
        ++packets;
        if (drop_every && packets % drop_every == 0) {
            ++dropped_packets;
            dropped_msgs += packer.count();
        } else {
            if (pcap) pcap->write(p.data(), p.size(), last_ts);
            batch.push_back(p);
            if (batch.size() == kBatch) flush();
        }
        packer.next();
        if (rate > 0) {  // pace on messages handed out so far
            const auto due = t0 + std::chrono::duration<double>(static_cast<double>(messages) / rate);
            while (Clock::now() < due) {
            }
        }
    };

    itch::for_each_message(f.data(), f.size(), [&](const std::uint8_t* m, std::size_t len) {
        if (messages >= limit) return;
        if (len >= 11) last_ts = load_be48(m + 5);
        if (!packer.add(m, len)) {
            emit_packet();
            packer.add(m, len);
        }
        ++messages;
    });
    emit_packet();
    flush();
    for (int k = 0; k < 3; ++k) {
        const auto eos = packer.end_of_session();
        if (pcap && k == 0) pcap->write(eos.data(), eos.size(), last_ts);
        if (send) {
            ::sendto(fd, eos.data(), eos.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();

    std::printf("messages      %zu\n", messages);
    std::printf("packets       %zu (%zu sent, %zu dropped on purpose holding %zu messages)\n",
                packets, sent, dropped_packets, dropped_msgs);
    if (pcap) std::printf("pcap          %llu frames -> %s\n", (unsigned long long)pcap->frames(), pcap_path.c_str());
    if (send) std::printf("send          %.3f s, %.2f M msgs/s, %zu send retries\n", sec,
                          static_cast<double>(messages) / sec / 1e6, send_errors);
    if (fd >= 0) ::close(fd);
    return 0;
}
