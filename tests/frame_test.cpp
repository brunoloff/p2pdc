#include "p2pdc/frame.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace p2pdc;

namespace {
Bytes from_hex(std::string_view hex) {
    Bytes bytes;
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(
            static_cast<std::byte>(std::stoul(std::string(hex.substr(index, 2)), nullptr, 16)));
    }
    return bytes;
}
} // namespace

TEST(Frame, PublishedVersionOneVectors) {
    const FrameLimits limits{1024, 1024};
#define FRAME_VECTOR(name, id, total, fragment_offset, payload_hex, wire_hex)                      \
    {                                                                                              \
        SCOPED_TRACE(#name);                                                                       \
        const Frame frame{id, total, fragment_offset, from_hex(payload_hex)};                      \
        const auto expected = from_hex(wire_hex);                                                  \
        const auto encoded = encode_frame(frame, limits);                                          \
        ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));                                       \
        EXPECT_EQ(std::get<Bytes>(encoded), expected);                                             \
        const auto decoded = decode_frame(expected, limits);                                       \
        ASSERT_TRUE(std::holds_alternative<Frame>(decoded));                                       \
        const auto &actual = std::get<Frame>(decoded);                                             \
        EXPECT_EQ(actual.message_id, frame.message_id);                                            \
        EXPECT_EQ(actual.total_size, frame.total_size);                                            \
        EXPECT_EQ(actual.offset, frame.offset);                                                    \
        EXPECT_EQ(actual.payload, frame.payload);                                                  \
    }
#include "frame_vectors.inc"
#undef FRAME_VECTOR
}

TEST(Frame, ViewBorrowsPayloadAndRejectsMalformedInput) {
    const FrameLimits limits{4, 64};
    auto bytes =
        std::get<Bytes>(encode_frame(Frame{2, 2, 0, {std::byte{1}, std::byte{2}}}, limits));
    const auto decoded = decode_frame_view(bytes, limits);
    ASSERT_TRUE(std::holds_alternative<FrameView>(decoded));
    const auto &view = std::get<FrameView>(decoded);
    EXPECT_EQ(view.payload.data(), bytes.data() + frame_header_size);
    bytes.back() = std::byte{3};
    EXPECT_EQ(view.payload.back(), std::byte{3});
    EXPECT_EQ(std::get<FrameError>(decode_frame_view({}, limits)), FrameError::Truncated);
}

TEST(Frame, RoundTripsAllByteValuesAndNetworkOrder) {
    Bytes payload;
    for (unsigned int i = 0; i < 256; ++i) {
        payload.push_back(static_cast<std::byte>(i));
    }
    const Frame frame{0x0102030405060708ULL, 256, 0, payload};
    const FrameLimits limits{256, 1024};
    const auto encoded = encode_frame(frame, limits);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    const auto &bytes = std::get<Bytes>(encoded);
    EXPECT_EQ(bytes[3], std::byte{1});
    EXPECT_EQ(bytes[10], std::byte{8});
    auto decoded = decode_frame(bytes, limits);
    ASSERT_TRUE(std::holds_alternative<Frame>(decoded));
    EXPECT_EQ(std::get<Frame>(decoded).message_id, frame.message_id);
    EXPECT_EQ(std::get<Frame>(decoded).payload, payload);
}

TEST(Frame, SupportsEmptyMessageAndRejectsEmptyNonemptyFragment) {
    const FrameLimits limits{100, 100};
    auto empty = encode_frame(Frame{0, 0, 0, {}}, limits);
    ASSERT_TRUE(std::holds_alternative<Bytes>(empty));
    EXPECT_TRUE(std::holds_alternative<Frame>(decode_frame(std::get<Bytes>(empty), limits)));
    EXPECT_EQ(std::get<FrameError>(encode_frame(Frame{1, 1, 0, {}}, limits)),
              FrameError::EmptyFragment);
}

TEST(Frame, RejectsInvalidBoundsAndLimitsBeforeEncoding) {
    const FrameLimits limits{10, 20};
    EXPECT_EQ(std::get<FrameError>(encode_frame(Frame{1, 11, 0, {std::byte{1}}}, limits)),
              FrameError::TooLarge);
    EXPECT_EQ(std::get<FrameError>(encode_frame(Frame{1, 1, 2, {std::byte{1}}}, limits)),
              FrameError::InvalidBounds);
    EXPECT_EQ(std::get<FrameError>(
                  encode_frame(Frame{1, 2, 1, {std::byte{1}, std::byte{2}}}, FrameLimits{10, 100})),
              FrameError::InvalidBounds);
    EXPECT_EQ(
        std::get<FrameError>(encode_frame(Frame{1, 1, 0, {std::byte{1}}}, FrameLimits{10, 18})),
        FrameError::TooLarge);
}

TEST(Frame, RejectsMalformedAndOversizedInput) {
    const FrameLimits limits{100, 100};
    EXPECT_EQ(std::get<FrameError>(decode_frame(Bytes{}, limits)), FrameError::Truncated);
    auto encoded = std::get<Bytes>(encode_frame(Frame{7, 1, 0, {std::byte{9}}}, limits));
    auto bad = encoded;
    bad[0] = std::byte{0};
    EXPECT_EQ(std::get<FrameError>(decode_frame(bad, limits)), FrameError::InvalidMagic);
    bad = encoded;
    bad[2] = std::byte{2};
    EXPECT_EQ(std::get<FrameError>(decode_frame(bad, limits)), FrameError::InvalidVersion);
    EXPECT_EQ(std::get<FrameError>(decode_frame(encoded, FrameLimits{100, 19})),
              FrameError::TooLarge);
    EXPECT_EQ(std::get<FrameError>(decode_frame(encoded, FrameLimits{0, 100})),
              FrameError::TooLarge);
    bad = encoded;
    bad[18] = std::byte{2};
    EXPECT_EQ(std::get<FrameError>(decode_frame(bad, limits)), FrameError::InvalidBounds);
}

TEST(Frame, RejectsPayloadLargerThanDeclaredMessage) {
    auto bytes = std::get<Bytes>(encode_frame(Frame{1, 1, 0, {std::byte{1}}}, {1, 100000}));
    bytes.resize(100000, std::byte{1});
    EXPECT_EQ(std::get<FrameError>(decode_frame(bytes, {1, 100000})), FrameError::InvalidBounds);
}

TEST(Frame, SpanEncoderCopiesAndValidatesFragment) {
    Bytes input{std::byte{3}, std::byte{4}};
    const FrameLimits limits{2, 100};
    auto encoded = encode_fragment(8, 2, 0, input, limits);
    ASSERT_TRUE(std::holds_alternative<Bytes>(encoded));
    input[0] = std::byte{5};
    const auto decoded = decode_frame(std::get<Bytes>(encoded), limits);
    ASSERT_TRUE(std::holds_alternative<Frame>(decoded));
    EXPECT_EQ(std::get<Frame>(decoded).payload, (Bytes{std::byte{3}, std::byte{4}}));
    EXPECT_EQ(std::get<FrameError>(encode_fragment(8, 1, 0, input, limits)),
              FrameError::InvalidBounds);
}
