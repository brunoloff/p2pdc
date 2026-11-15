#include "p2pdc/node.h"

#include <gtest/gtest.h>

using namespace p2pdc;

TEST(NodeIdentityDisabledTest, ReportsUnsupportedAndValidatesExecutor) {
    const auto executor = [](std::function<void()>) {};
    Bytes seed(32), secret(32);
    auto result = Node::create_with_identity({}, executor, seed, secret);
    ASSERT_TRUE(std::holds_alternative<NodeIdentityError>(result));
    EXPECT_EQ(std::get<NodeIdentityError>(result), NodeIdentityError::Unsupported);
    EXPECT_THROW(static_cast<void>(Node::create_with_identity({}, {}, seed, secret)),
                 std::invalid_argument);
}
