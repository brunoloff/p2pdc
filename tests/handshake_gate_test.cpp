#include "../src/security/handshake_gate.hpp"
#include "borrowed.hpp"

#include <gtest/gtest.h>

#include <new>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
struct Transport : HandshakeTransport {
    bool blocked = false;
    bool closed = false;
    std::vector<Bytes> sent;
    std::vector<std::size_t> limits;
    ChannelSend send(ByteView bytes) override {
        if (closed) {
            return ChannelSend::Closed;
        }
        if (blocked) {
            return ChannelSend::Backpressure;
        }
        sent.emplace_back(bytes.begin(), bytes.end());
        return ChannelSend::Accepted;
    }
    void limit_receive(std::size_t bytes) override { limits.push_back(bytes); }
    void close() override { closed = true; }
};

struct Pair {
    Identity a = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    Identity b = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2})));
    Transport left;
    Transport right;
    std::unique_ptr<HandshakeGate> ga;
    std::unique_ptr<HandshakeGate> gb;
    std::vector<Bytes> received;
    std::vector<GateError> errors;
    int ready_count = 0;
    HandshakeContext context() const {
        return {a.public_key(), b.public_key(), {std::byte{1}}, {std::byte{2}},
                {std::byte{3}}, {std::byte{4}}, {std::byte{5}}, {std::byte{6}}};
    }
    HandshakeGateCallbacks callbacks() {
        return {[this](std::uint32_t limit) {
                    EXPECT_EQ(limit, 80U);
                    ++ready_count;
                },
                [this](Bytes message) { received.push_back(std::move(message)); }, [] {},
                [this](GateError error) { errors.push_back(error); }};
    }
    void create() {
        ga = std::make_unique<HandshakeGate>(borrowed(a), context(), HandshakeRole::Offerer, 100,
                                             200, left, callbacks());
        gb = std::make_unique<HandshakeGate>(borrowed(b), context(), HandshakeRole::Answerer, 80,
                                             200, right, callbacks());
    }
    void deliver() {
        for (int count = 0; count < 16 && (!left.sent.empty() || !right.sent.empty()); ++count) {
            if (!left.sent.empty()) {
                auto frame = std::move(left.sent.front());
                left.sent.erase(left.sent.begin());
                gb->on_message(std::move(frame));
            }
            if (!right.sent.empty()) {
                auto frame = std::move(right.sent.front());
                right.sent.erase(right.sent.begin());
                ga->on_message(std::move(frame));
            }
        }
    }
};
} // namespace

TEST(HandshakeGate, WithholdsDataUntilBothConfirmAndNegotiatesLimits) {
    Pair pair;
    pair.create();
    EXPECT_EQ(pair.ga->send({}), ChannelSend::NotReady);
    pair.ga->set_transport_ready(true);
    pair.gb->set_transport_ready(true);
    EXPECT_EQ(pair.ga->send({}), ChannelSend::NotReady);
    pair.deliver();
    EXPECT_TRUE(pair.ga->ready());
    EXPECT_TRUE(pair.gb->ready());
    EXPECT_EQ(pair.ready_count, 2);
    EXPECT_EQ(pair.left.limits, (std::vector<std::size_t>{1 + handshake_record_size, 200}));
    EXPECT_EQ(pair.right.limits, (std::vector<std::size_t>{1 + handshake_record_size, 200}));
    EXPECT_EQ(pair.ga->send(Bytes(81)), ChannelSend::TooLarge);
    EXPECT_EQ(pair.ga->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    pair.deliver();
    ASSERT_EQ(pair.received.size(), 1U);
    EXPECT_EQ(pair.received[0], (Bytes{std::byte{0}, std::byte{255}}));
    EXPECT_TRUE(pair.errors.empty());
}

TEST(HandshakeGate, DeliversApplicationBytesWithoutCopyingThem) {
    Pair pair;
    pair.create();
    pair.ga->set_transport_ready(true);
    pair.gb->set_transport_ready(true);
    pair.deliver();
    ASSERT_TRUE(pair.gb->ready());
    Bytes frame{std::byte{1}, std::byte{4}, std::byte{5}, std::byte{6}};
    const auto *storage = frame.data();
    pair.gb->on_message(std::move(frame));
    ASSERT_EQ(pair.received.size(), 1U);
    EXPECT_EQ(pair.received[0], (Bytes{std::byte{4}, std::byte{5}, std::byte{6}}));
    EXPECT_EQ(pair.received[0].data(), storage);
}

TEST(HandshakeGate, RetriesControlAfterBackpressureAndSuspension) {
    Pair pair;
    pair.create();
    pair.left.blocked = true;
    pair.ga->set_transport_ready(true);
    pair.gb->set_transport_ready(true);
    pair.deliver();
    EXPECT_FALSE(pair.ga->ready());
    EXPECT_FALSE(pair.gb->ready());
    pair.ga->set_transport_ready(false);
    pair.left.blocked = false;
    pair.ga->on_writable();
    EXPECT_TRUE(pair.left.sent.empty());
    pair.ga->set_transport_ready(true);
    pair.deliver();
    EXPECT_TRUE(pair.ga->ready());
    EXPECT_TRUE(pair.gb->ready());
    EXPECT_TRUE(pair.errors.empty());
}

TEST(HandshakeGate, RejectsPrematureDataAndHonorsTimeout) {
    Pair pair;
    pair.create();
    pair.ga->set_transport_ready(true);
    pair.ga->on_message(Bytes{std::byte{1}, std::byte{7}});
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors[0], GateError::Protocol);
    EXPECT_TRUE(pair.left.closed);
    EXPECT_EQ(pair.ga->send({}), ChannelSend::Closed);
    pair.gb->timeout();
    ASSERT_EQ(pair.errors.size(), 2U);
    EXPECT_EQ(pair.errors[1], GateError::Timeout);
    EXPECT_TRUE(pair.right.closed);
}

TEST(HandshakeGate, RejectsInvalidLimitsAndCallbackDestructionIsSafe) {
    Pair pair;
    EXPECT_THROW((HandshakeGate{borrowed(pair.a), pair.context(), HandshakeRole::Offerer, 100, 139,
                                pair.left, pair.callbacks()}),
                 std::invalid_argument);
    auto callbacks = pair.callbacks();
    callbacks.on_error = [&](GateError) { pair.ga.reset(); };
    pair.ga =
        std::make_unique<HandshakeGate>(borrowed(pair.a), pair.context(), HandshakeRole::Offerer,
                                        100, 200, pair.left, std::move(callbacks));
    pair.ga->set_transport_ready(true);
    pair.ga->on_message({});
    EXPECT_FALSE(pair.ga);
    EXPECT_TRUE(pair.left.closed);
}

TEST(HandshakeGate, AllocationFailureDuringVerificationClosesAndReportsError) {
    Pair pair;
    pair.ga = std::make_unique<HandshakeGate>(borrowed(pair.a), pair.context(),
                                              HandshakeRole::Offerer, 100, 200, pair.left,
                                              pair.callbacks(), [] { throw std::bad_alloc(); });
    pair.ga->set_transport_ready(true);
    pair.ga->on_message(Bytes{std::byte{0}});
    ASSERT_EQ(pair.errors.size(), 1U);
    EXPECT_EQ(pair.errors.front(), GateError::Allocation);
    EXPECT_TRUE(pair.left.closed);
    EXPECT_EQ(pair.ga->send({}), ChannelSend::Closed);
}
