#include "../../tests/native_public_incoming_fixture.hpp"
#include "reference_document.hpp"

namespace {
using p2pdc_yrs_reference::Document;

class YrsReferenceTest : public NativePublicIncomingTest {
  protected:
    void open_pair() {
        listen();
        connect_sender();
        ASSERT_TRUE(advance([&] { return request.has_value() || !incoming_errors.empty(); }));
        ASSERT_TRUE(incoming_errors.empty());
        auto result = receiver.node->accept(*incoming, {}, scheduler(), {}, handlers(received));
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Connection>>(result));
        receiving = std::move(std::get<std::unique_ptr<Connection>>(result));
        ASSERT_TRUE(advance([&] {
            return (sending->state() == ConnectionState::Ready &&
                    receiving->state() == ConnectionState::Ready) ||
                   !connection_errors.empty();
        }));
        ASSERT_TRUE(connection_errors.empty());
    }
    void close_pair() {
        sending->close();
        receiving->close();
        incoming->close();
        sending.reset();
        receiving.reset();
        incoming.reset();
        request.reset();
    }
    bool transfer(Connection &connection, std::vector<Bytes> &mailbox, const Bytes &value,
                  Bytes &result) {
        mailbox.clear();
        if (connection.send(value) != SendStatus::Accepted ||
            !advance([&] { return !mailbox.empty() || !connection_errors.empty(); }) ||
            !connection_errors.empty() || mailbox.size() != 1)
            return false;
        result = std::move(mailbox.front());
        return true;
    }
    void sync(Document &left, Document &right) {
        Bytes left_vector, right_vector, left_update, right_update;
        ASSERT_TRUE(transfer(*sending, received, left.state_vector(), left_vector));
        ASSERT_TRUE(transfer(*receiving, sent, right.state_vector(), right_vector));
        ASSERT_TRUE(transfer(*sending, received, left.difference(right_vector), left_update));
        ASSERT_TRUE(transfer(*receiving, sent, right.difference(left_vector), right_update));
        ASSERT_TRUE(left.apply(right_update));
        ASSERT_TRUE(right.apply(left_update));
    }
};

TEST_F(YrsReferenceTest, OfflineEditsConvergeAfterFreshConnection) {
    ASSERT_NO_FATAL_FAILURE(setup_sides());
    ASSERT_NO_FATAL_FAILURE(open_pair());
    Document left, right;
    left.append("shared");
    ASSERT_NO_FATAL_FAILURE(sync(left, right));
    ASSERT_EQ(left.text(), "shared");
    ASSERT_EQ(right.text(), left.text());
    close_pair();
    left.append("-left");
    right.append("-right");
    ASSERT_NE(left.text(), right.text());
    ASSERT_NO_FATAL_FAILURE(open_pair());
    ASSERT_NO_FATAL_FAILURE(sync(left, right));
    EXPECT_EQ(left.text(), right.text());
    EXPECT_NE(left.text().find("-left"), std::string::npos);
    EXPECT_NE(left.text().find("-right"), std::string::npos);
    close_pair();
}

TEST(YrsReferenceDocumentTest, AppliesDifferencesAndRejectsMalformedUpdate) {
    Document source, target;
    source.append("text");
    ASSERT_TRUE(target.apply(source.difference(target.state_vector())));
    EXPECT_EQ(target.text(), "text");
    EXPECT_THROW(static_cast<void>(source.difference({})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(source.difference(Bytes{std::byte{0xff}})), std::runtime_error);
    EXPECT_FALSE(target.apply({}));
    EXPECT_FALSE(target.apply(Bytes{std::byte{0xff}}));
    EXPECT_EQ(target.text(), "text");
}
} // namespace
