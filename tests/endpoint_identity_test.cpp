#include "../src/discovery/endpoint_identity.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

TEST(EndpointIdentity, GeneratesMatchingKeysAndDistinctAnnouncementInstances) {
    auto result = make_endpoint_identity();
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<const EndpointIdentity>>(result));
    auto identity = std::get<std::shared_ptr<const EndpointIdentity>>(std::move(result));
    const auto first = make_announcement_endpoint(identity);
    const auto second = make_announcement_endpoint(identity);
    EXPECT_EQ(first.fields.recipient_public_key, identity->recipient->public_key());
    EXPECT_NE(first.fields.instance_id, second.fields.instance_id);
    EXPECT_NE(first.fields.signaling_inbox, second.fields.signaling_inbox);
    Bytes message{std::byte{1}, std::byte{2}};
    auto sealed = seal_to_recipient(first.fields.recipient_public_key, message, 128);
    ASSERT_TRUE(std::holds_alternative<Bytes>(sealed));
    auto opened = identity->recipient->open_sealed(std::get<Bytes>(sealed), 128);
    ASSERT_TRUE(std::holds_alternative<Bytes>(opened));
    EXPECT_EQ(std::get<Bytes>(opened), message);
}

TEST(EndpointIdentity, RejectsMissingIdentity) {
    EXPECT_THROW(static_cast<void>(make_announcement_endpoint({})), std::invalid_argument);
}
