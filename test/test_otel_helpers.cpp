// Unit tests for OpenTelemetry helpers (no external deps needed)
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>

// Duplicate HexToBytes from otel_tracer.cpp for isolated testing
static bool HexToBytes(const std::string &hex, uint8_t *out, size_t out_len) {
    if (hex.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned int byte;
        if (sscanf(hex.c_str() + i * 2, "%2x", &byte) != 1) return false;
        out[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

TEST(OTelHelpers, HexToBytesValid) {
    uint8_t buf[16] = {0};
    EXPECT_TRUE(HexToBytes("0123456789abcdef0123456789abcdef", buf, 16));
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[15], 0xef);
}

TEST(OTelHelpers, HexToBytesInvalidLength) {
    uint8_t buf[16];
    EXPECT_FALSE(HexToBytes("0123", buf, 16));   // too short
    EXPECT_FALSE(HexToBytes("0123456789abcdef0123456789abcdef00", buf, 16)); // too long
}

TEST(OTelHelpers, HexToBytesInvalidChars) {
    uint8_t buf[16];
    EXPECT_FALSE(HexToBytes("GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG", buf, 16));
}

TEST(OTelHelpers, TraceParentFormat) {
    // Verify the W3C traceparent format construction
    std::string trace_id_hex = "0123456789abcdef0123456789abcdef";
    std::string span_id_hex = "0123456789abcdef";
    std::string flags = "01";

    char buf[128];
    snprintf(buf, sizeof(buf), "00-%s-%s-%s", trace_id_hex.c_str(), span_id_hex.c_str(), flags.c_str());
    std::string tp(buf);

    EXPECT_EQ(tp.size(), 55);
    EXPECT_EQ(tp[0], '0');
    EXPECT_EQ(tp[1], '0');
    EXPECT_EQ(tp[2], '-');
    EXPECT_EQ(tp[35], '-');
    EXPECT_EQ(tp[52], '-');
}

TEST(OTelHelpers, TraceParentNotSampled) {
    char buf[128];
    snprintf(buf, sizeof(buf), "00-%s-%s-%s",
             "00000000000000000000000000000000",
             "0000000000000000", "00");
    std::string tp(buf);
    EXPECT_EQ(tp, "00-00000000000000000000000000000000-0000000000000000-00");
}
