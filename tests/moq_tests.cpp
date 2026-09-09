// The MoQ wire format.
//
// A separately deployed QUIC server reads these bytes, so the format is a
// PUBLISHED CONTRACT with the same standing as the AI result socket: it cannot
// change without coordinating a deployment. Pinned here byte by byte, because
// "it still looks right" is not a check.

#include "TestHarness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "media/moq/MoqFeed.hpp"

using namespace visora::media;

namespace {

std::string hex(const std::vector<std::uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 0x0F];
    }
    return out;
}

}  // namespace

VS_TEST(a_frame_header_is_thirteen_bytes_big_endian) {
    // flags(1) | pts_us(8) | length(4), all big-endian. A reader indexes into
    // these by fixed offset, so every byte is part of the contract.
    const auto header = moqFrameHeader(/*ptsUs=*/0x0102030405060708ULL,
                                       /*length=*/0x0A0B0C0DU, /*keyframe=*/true);
    VS_CHECK_EQ(header.size(), kMoqFrameHeaderBytes);
    VS_CHECK(hex(header) == "01" "0102030405060708" "0a0b0c0d");
}

VS_TEST(the_keyframe_flag_is_bit_zero_and_nothing_else_is_set) {
    const auto key = moqFrameHeader(0, 0, true);
    const auto delta = moqFrameHeader(0, 0, false);
    VS_CHECK_EQ(key[0], std::uint8_t{0x01});
    VS_CHECK_EQ(delta[0], std::uint8_t{0x00});
}

VS_TEST(a_zero_frame_is_all_zeroes_except_the_flag) {
    VS_CHECK(hex(moqFrameHeader(0, 0, false)) == "00" "0000000000000000" "00000000");
}

VS_TEST(large_values_are_not_truncated) {
    // A 64-bit pts and a length near 4 GB must survive intact — a reader that
    // gets a truncated length loses frame alignment for the rest of the
    // session, not just for that frame.
    const auto header = moqFrameHeader(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFU, false);
    VS_CHECK(hex(header) == "00" "ffffffffffffffff" "ffffffff");
}

VS_TEST(the_session_header_is_the_documented_line) {
    const std::string header = moqSessionHeader("feed-1", "cam-9", "sess-3", "h265");
    VS_CHECK(header ==
             "MOQF1 {\"feed\":\"feed-1\",\"camera\":\"cam-9\",\"session\":\"sess-3\","
             "\"codec\":\"h265\"}\n");
    // The trailing newline is what the reader frames on.
    VS_CHECK(!header.empty() && header.back() == '\n');
}

VS_TEST(a_quote_in_an_id_cannot_break_the_header) {
    // The ids come from a request body. An unescaped quote would produce a
    // header line the reader cannot parse, and it would fail on the session
    // rather than on the request that caused it.
    const std::string header = moqSessionHeader("a\"b", "cam", "s", "h264");
    VS_CHECK(header.find("\"feed\":\"a\\\"b\"") != std::string::npos);
}

VS_MAIN()
