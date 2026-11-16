#include "../src/security/secret_bytes.hpp"

#include <gtest/gtest.h>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(SecretBytes, CopiesInputAndAttachesTheWipingDeleter) {
    const Bytes input{std::byte{1}, std::byte{2}, std::byte{3}};
    auto secret = make_secret_bytes(input);
    ASSERT_TRUE(secret);
    EXPECT_EQ(*secret, input);
    EXPECT_NE(std::get_deleter<SecretBytesDeleter>(secret), nullptr);
    EXPECT_TRUE(make_secret_bytes({})->empty());
}

TEST(SecretBytes, WipeZeroesEveryByte) {
    Bytes value(32, std::byte{0xAB});
    wipe_bytes(value);
    EXPECT_EQ(value, Bytes(32, std::byte{0}));
    Bytes empty;
    wipe_bytes(empty);
    EXPECT_TRUE(empty.empty());
}
