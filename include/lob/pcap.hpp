#pragma once
// Writes UDP datagrams as Ethernet/IPv4/UDP frames into a classic pcap file
// (readable by Wireshark, tcpdump, scapy). Used to hand the exact wire bytes of
// a replay to the FPGA parser's test bench.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/endian.hpp"

namespace lob {

inline std::uint16_t ipv4_checksum(const std::uint8_t* hdr, std::size_t n) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) sum += load_be16(hdr + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
}

class PcapWriter {
   public:
    PcapWriter(const std::string& path, std::uint32_t src_ip, std::uint32_t dst_ip,
               std::uint16_t src_port, std::uint16_t dst_port)
        : src_ip_(src_ip), dst_ip_(dst_ip), sport_(src_port), dport_(dst_port) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) throw std::runtime_error("cannot write " + path);
        // Global header (host byte order; the magic tells readers which order):
        // magic, version 2.4, timezone 0, sigfigs 0, snaplen 65535, link type 1 (Ethernet)
        std::uint8_t hdr[24];
        const std::uint32_t magic = 0xa1b2c3d4u, zero = 0, snaplen = 65535, linktype = 1;
        const std::uint16_t vmaj = 2, vmin = 4;
        std::memcpy(hdr, &magic, 4);
        std::memcpy(hdr + 4, &vmaj, 2);
        std::memcpy(hdr + 6, &vmin, 2);
        std::memcpy(hdr + 8, &zero, 4);
        std::memcpy(hdr + 12, &zero, 4);
        std::memcpy(hdr + 16, &snaplen, 4);
        std::memcpy(hdr + 20, &linktype, 4);
        std::fwrite(hdr, 1, sizeof hdr, f_);
    }
    PcapWriter(const PcapWriter&) = delete;
    PcapWriter& operator=(const PcapWriter&) = delete;
    ~PcapWriter() {
        if (f_) std::fclose(f_);
    }

    // ts_ns: capture timestamp (ns since midnight is fine for replays).
    void write(const std::uint8_t* payload, std::size_t n, std::uint64_t ts_ns) {
        frame_.assign(42 + n, 0);
        std::uint8_t* e = frame_.data();
        // Ethernet: multicast MAC 01:00:5e + low 23 bits of the group address
        const std::uint8_t dmac[6] = {0x01, 0x00, 0x5e, static_cast<std::uint8_t>((dst_ip_ >> 16) & 0x7f),
                                      static_cast<std::uint8_t>(dst_ip_ >> 8), static_cast<std::uint8_t>(dst_ip_)};
        const std::uint8_t smac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        std::memcpy(e, dmac, 6);
        std::memcpy(e + 6, smac, 6);
        store_be16(e + 12, 0x0800);
        // IPv4
        std::uint8_t* ip = e + 14;
        ip[0] = 0x45;
        store_be16(ip + 2, static_cast<std::uint16_t>(28 + n));
        store_be16(ip + 4, ip_id_++);
        ip[8] = 32;   // TTL
        ip[9] = 17;   // UDP
        store_be32(ip + 12, src_ip_);
        store_be32(ip + 16, dst_ip_);
        store_be16(ip + 10, ipv4_checksum(ip, 20));
        // UDP (checksum 0 = not used, allowed for IPv4)
        std::uint8_t* udp = ip + 20;
        store_be16(udp, sport_);
        store_be16(udp + 2, dport_);
        store_be16(udp + 4, static_cast<std::uint16_t>(8 + n));
        std::memcpy(udp + 8, payload, n);
        // pcap record header: ts_sec, ts_usec, incl_len, orig_len (host order)
        const std::uint32_t rec[4] = {static_cast<std::uint32_t>(ts_ns / 1'000'000'000ULL),
                                      static_cast<std::uint32_t>((ts_ns / 1000) % 1'000'000),
                                      static_cast<std::uint32_t>(frame_.size()),
                                      static_cast<std::uint32_t>(frame_.size())};
        std::fwrite(rec, sizeof rec, 1, f_);
        std::fwrite(frame_.data(), 1, frame_.size(), f_);
        ++frames_;
    }

    std::uint64_t frames() const noexcept { return frames_; }

   private:
    std::FILE* f_ = nullptr;
    std::uint32_t src_ip_, dst_ip_;
    std::uint16_t sport_, dport_;
    std::uint16_t ip_id_ = 1;
    std::uint64_t frames_ = 0;
    std::vector<std::uint8_t> frame_;
};

}  // namespace lob
