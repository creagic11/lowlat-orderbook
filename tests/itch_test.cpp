#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "lob/endian.hpp"
#include "lob/itch.hpp"
#include "lob/itch_file.hpp"

using namespace lob;
using namespace lob::itch;

namespace {

// Builds a message of the right size with the common header filled in.
std::vector<std::uint8_t> msg(char type, std::uint16_t locate, std::uint64_t ts) {
    std::vector<std::uint8_t> m(kMessageSize[static_cast<unsigned char>(type)], 0);
    m[0] = static_cast<std::uint8_t>(type);
    store_be16(&m[1], locate);
    store_be16(&m[3], 7);  // tracking number
    store_be48(&m[5], ts);
    return m;
}

constexpr std::uint64_t kTs = 34'200'123'456'789ULL;  // 09:30:00.123456789

}  // namespace

TEST(Endian, RoundTrips) {
    std::array<std::uint8_t, 8> b{};
    store_be48(b.data(), 0x0000'1234'5678'9ABCULL);
    EXPECT_EQ(b[0], 0x12);
    EXPECT_EQ(b[5], 0xBC);
    EXPECT_EQ(load_be48(b.data()), 0x1234'5678'9ABCULL);
    store_be64(b.data(), 0x0102030405060708ULL);
    EXPECT_EQ(b[0], 0x01);
    EXPECT_EQ(load_be64(b.data()), 0x0102030405060708ULL);
    store_be32(b.data(), 0xDEADBEEF);
    EXPECT_EQ(load_be32(b.data()), 0xDEADBEEFu);
}

TEST(Itch, AddOrder) {
    auto m = msg('A', 42, kTs);
    store_be64(&m[11], 987654321ULL);
    m[19] = 'B';
    store_be32(&m[20], 300);
    std::memcpy(&m[24], "AAPL    ", 8);
    store_be32(&m[32], 1'234'500);  // $123.45
    Event ev;
    ASSERT_EQ(decode(m.data(), m.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::Add);
    EXPECT_EQ(ev.locate, 42);
    EXPECT_EQ(ev.ts, kTs);
    EXPECT_EQ(ev.ref, 987654321ULL);
    EXPECT_EQ(ev.side, 'B');
    EXPECT_EQ(ev.shares, 300u);
    EXPECT_EQ(ev.price, 1'234'500u);
    EXPECT_EQ(add_stock(m.data()), "AAPL    ");
}

TEST(Itch, AddOrderWithMpidIsAnAdd) {
    auto m = msg('F', 1, kTs);
    store_be64(&m[11], 5);
    m[19] = 'S';
    store_be32(&m[20], 100);
    store_be32(&m[32], 500);
    std::memcpy(&m[36], "GSCO", 4);
    Event ev;
    ASSERT_EQ(decode(m.data(), m.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::Add);
    EXPECT_EQ(ev.side, 'S');
    EXPECT_EQ(ev.msg_type, 'F');
}

TEST(Itch, Executions) {
    auto e = msg('E', 3, kTs);
    store_be64(&e[11], 77);
    store_be32(&e[19], 150);
    store_be64(&e[23], 999);  // match number, ignored by the book
    Event ev;
    ASSERT_EQ(decode(e.data(), e.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::Execute);
    EXPECT_EQ(ev.ref, 77u);
    EXPECT_EQ(ev.shares, 150u);

    auto c = msg('C', 3, kTs);
    store_be64(&c[11], 78);
    store_be32(&c[19], 40);
    c[31] = 'N';
    store_be32(&c[32], 1'000'100);
    ASSERT_EQ(decode(c.data(), c.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::ExecutePrice);
    EXPECT_EQ(ev.ref, 78u);
    EXPECT_EQ(ev.shares, 40u);
    EXPECT_EQ(ev.price, 1'000'100u);
}

TEST(Itch, CancelDeleteReplace) {
    Event ev;
    auto x = msg('X', 9, kTs);
    store_be64(&x[11], 11);
    store_be32(&x[19], 25);
    ASSERT_EQ(decode(x.data(), x.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::Cancel);
    EXPECT_EQ(ev.shares, 25u);

    auto d = msg('D', 9, kTs);
    store_be64(&d[11], 12);
    ASSERT_EQ(decode(d.data(), d.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::Delete);
    EXPECT_EQ(ev.ref, 12u);

    auto u = msg('U', 9, kTs);
    store_be64(&u[11], 13);
    store_be64(&u[19], 14);
    store_be32(&u[27], 600);
    store_be32(&u[31], 2'000'000);
    ASSERT_EQ(decode(u.data(), u.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::Replace);
    EXPECT_EQ(ev.ref, 13u);
    EXPECT_EQ(ev.new_ref, 14u);
    EXPECT_EQ(ev.shares, 600u);
    EXPECT_EQ(ev.price, 2'000'000u);
}

TEST(Itch, TradeIsReportedButNotABookChange) {
    auto p = msg('P', 2, kTs);
    p[19] = 'B';
    store_be32(&p[20], 100);
    store_be32(&p[32], 450'000);
    Event ev;
    ASSERT_EQ(decode(p.data(), p.size(), ev), Decode::BookEvent);
    EXPECT_EQ(ev.type, EventType::Trade);
    EXPECT_EQ(ev.price, 450'000u);
}

TEST(Itch, NonBookAndInvalidMessages) {
    Event ev;
    auto s = msg('S', 0, kTs);
    s[11] = 'Q';
    EXPECT_EQ(decode(s.data(), s.size(), ev), Decode::Other);
    EXPECT_EQ(system_event_code(s.data(), s.size()), 'Q');

    auto r = msg('R', 17, kTs);
    std::memcpy(&r[11], "MSFT    ", 8);
    store_be32(&r[21], 100);
    StockDirectory sd{};
    ASSERT_TRUE(decode_stock_directory(r.data(), r.size(), sd));
    EXPECT_EQ(sd.locate, 17);
    EXPECT_STREQ(sd.stock, "MSFT");
    EXPECT_EQ(sd.round_lot, 100u);

    auto a = msg('A', 1, kTs);
    EXPECT_EQ(decode(a.data(), a.size() - 1, ev), Decode::BadLength);
    std::uint8_t junk[5] = {'z', 0, 0, 0, 0};
    EXPECT_EQ(decode(junk, sizeof junk, ev), Decode::Unknown);
    EXPECT_EQ(decode(junk, 0, ev), Decode::BadLength);
}

TEST(ItchFile, FramingDetectsTruncation) {
    std::vector<std::uint8_t> buf = {0, 3, 'x', 'y', 'z', 0, 4, 'a'};
    std::size_t n = 0;
    auto r = for_each_message(buf.data(), buf.size(), [&](const std::uint8_t*, std::size_t len) {
        EXPECT_EQ(len, 3u);
        ++n;
    });
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(r.messages, 1u);
    EXPECT_TRUE(r.truncated);
}

// Decoding the synthetic file must reproduce the generator's ground truth exactly.
TEST(ItchFile, SyntheticDayMatchesGroundTruth) {
    const std::string path = std::string(LOB_TEST_DATA_DIR) + "/synth_small.itch";
    std::map<std::string, std::size_t> truth;
    {
        std::ifstream in(path + ".counts");
        ASSERT_TRUE(in) << "missing " << path << ".counts";
        std::string k;
        std::size_t v;
        while (in >> k >> v) truth[k] = v;
    }
    MappedFile f(path);
    std::map<std::string, std::size_t> got;
    std::size_t bad = 0;
    auto r = for_each_message(f.data(), f.size(), [&](const std::uint8_t* m, std::size_t len) {
        Event ev;
        auto d = decode(m, len, ev);
        if (d == Decode::BadLength || d == Decode::Unknown) ++bad;
        ++got[std::string(1, static_cast<char>(m[0]))];
    });
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(r.messages, truth["total"]);
    for (const auto& [type, count] : got) EXPECT_EQ(count, truth[type]) << "type " << type;
}
