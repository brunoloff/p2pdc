#include "cursor.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

using namespace shared_text;
using namespace p2pdc;
TEST(SharedCursor, EncodesAndDecodesPositionAndColor) {
    const auto data = encodeCursor({65536, 0x12abcd});
    const auto cursor = decodeCursor(data);
    ASSERT_TRUE(cursor);
    EXPECT_EQ(cursor->position, 65536U);
    EXPECT_EQ(cursor->color, 0x12abcdU);
}
TEST(SharedCursor, RejectsMalformedOrOutOfRangeValues) {
    EXPECT_FALSE(decodeCursor(Bytes{std::byte{2}}));
    auto data = encodeCursor({1, 0});
    data[0] = std::byte{1};
    EXPECT_FALSE(decodeCursor(data));
    data = encodeCursor({1, 0});
    data[1] = std::byte{1};
    EXPECT_FALSE(decodeCursor(data));
    EXPECT_THROW((void)encodeCursor({65537, 0}), std::invalid_argument);
    EXPECT_THROW((void)encodeCursor({0, 0x1000000}), std::invalid_argument);
}
