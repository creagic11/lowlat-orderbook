#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "lob/itch_file.hpp"
#include "lob/moldudp64.hpp"
#include "lob/pcap.hpp"

using namespace lob;

namespace {
const char kSession[11] = "TESTSESS01";
}

TEST(MoldUDP64, PackAndParseRoundTrip) {
    mold::Packer packer(kSession, 1000, 100);
    std::vector<std::vector<std::uint8_t>> msgs;
    for (int i = 0; i < 30; ++i) msgs.emplace_back(static_cast<std::size_t>(1 + i % 13), static_cast<std::uint8_t>(i));

    std::vector<std::vector<std::uint8_t>> packets;
    for (const auto& m : msgs) {
        if (!packer.add(m.data(), m.size())) {
            packets.push_back(packer.finish());
            packer.next();
            ASSERT_TRUE(packer.add(m.data(), m.size()));
        }
    }
    packets.push_back(packer.finish());
    packer.next();

    std::uint64_t expect_seq = 1000;
    std::size_t k = 0;
    for (const auto& p : packets) {
        EXPECT_LE(p.size(), 100u);
        mold::Header h;
        ASSERT_TRUE(mold::parse_header(p.data(), p.size(), h));
        EXPECT_EQ(std::string(h.session, 10), "TESTSESS01");
        EXPECT_EQ(h.seq, expect_seq);
        ASSERT_TRUE(mold::for_each_message(p.data(), p.size(), h,
                                           [&](std::uint64_t seq, const std::uint8_t* m, std::size_t n) {
                                               EXPECT_EQ(seq, 1000 + k);
                                               ASSERT_EQ(std::vector<std::uint8_t>(m, m + n), msgs[k]);
                                               ++k;
                                           }));
        expect_seq += h.count;
    }
    EXPECT_EQ(k, msgs.size());

    const auto eos = packer.end_of_session();
    mold::Header h;
    ASSERT_TRUE(mold::parse_header(eos.data(), eos.size(), h));
    EXPECT_EQ(h.count, mold::kEndOfSession);
    EXPECT_EQ(h.seq, 1030u);
}

TEST(MoldUDP64, TruncatedPacketIsReported) {
    mold::Packer packer(kSession, 1, 1400);
    const std::uint8_t m[10] = {};
    packer.add(m, 10);
    packer.add(m, 10);
    auto p = packer.finish();
    p.resize(p.size() - 3);
    mold::Header h;
    ASSERT_TRUE(mold::parse_header(p.data(), p.size(), h));
    int n = 0;
    EXPECT_FALSE(mold::for_each_message(p.data(), p.size(), h,
                                        [&](std::uint64_t, const std::uint8_t*, std::size_t) { ++n; }));
    EXPECT_EQ(n, 1);
}

TEST(Pcap, Ipv4ChecksumKnownVector) {
    // Classic example header (checksum field zeroed) whose checksum is 0xB861.
    const std::uint8_t hdr[20] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                                  0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
    EXPECT_EQ(ipv4_checksum(hdr, 20), 0xB861);
}

TEST(Pcap, WritesValidFrames) {
    const std::string path = ::testing::TempDir() + "/lob_test.pcap";
    {
        PcapWriter w(path, 0x0A000001, 0xEF010203, 40000, 26477);  // 10.0.0.1 -> 239.1.2.3
        const std::uint8_t payload[5] = {1, 2, 3, 4, 5};
        w.write(payload, sizeof payload, 34'200'000'000'000ULL);
        EXPECT_EQ(w.frames(), 1u);
    }
    itch::MappedFile f(path);
    ASSERT_EQ(f.size(), 24u + 16u + 42u + 5u);
    const std::uint8_t* e = f.data() + 24 + 16;
    EXPECT_EQ(e[0], 0x01);  // multicast MAC 01:00:5e:01:02:03
    EXPECT_EQ(e[3], 0x01);
    EXPECT_EQ(e[5], 0x03);
    EXPECT_EQ(load_be16(e + 12), 0x0800);
    EXPECT_EQ(ipv4_checksum(e + 14, 20), 0) << "header checksum must verify to zero";
    EXPECT_EQ(load_be16(e + 14 + 2), 33);   // IP total length
    EXPECT_EQ(load_be16(e + 34 + 2), 26477);
    EXPECT_EQ(load_be16(e + 34 + 4), 13);   // UDP length
    EXPECT_EQ(e[42 + 4], 5);
    std::remove(path.c_str());
}
