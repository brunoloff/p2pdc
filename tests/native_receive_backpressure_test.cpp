#include "native_transport_pair.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace p2pdc::detail::testing;

namespace {
// More than the backends on both sides buffer, so some must wait in the sender.
constexpr std::uint32_t message_count = 8000;
constexpr std::size_t message_size = 1000;

Bytes numbered(std::uint32_t index) {
    Bytes bytes(message_size, std::byte{0});
    std::memcpy(bytes.data(), &index, sizeof index);
    return bytes;
}

std::uint32_t number_of(const Bytes &bytes) {
    std::uint32_t index = 0;
    std::memcpy(&index, bytes.data(), sizeof index);
    return index;
}

void run_for(Pair &pair, std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        pair.queue.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void send_all(Pair &pair) {
    for (std::uint32_t index = 0; index < message_count; ++index)
        ASSERT_EQ(pair.b->send(numbered(index)), NativeSendResult::Accepted);
}
} // namespace

// A receiver with no room takes nothing: the backlog backs up into the sender instead of
// failing the connection, and everything arrives in order once there is room again.
TEST(NativeReceiveBackpressure, HeldReceiveBacksUpThePeerWithoutFailing) {
    Pair pair;
    pair.create();
    ASSERT_TRUE(pair.connect());
    pair.hold_a = true;
    send_all(pair);
    run_for(pair, std::chrono::milliseconds(1000));
    EXPECT_TRUE(pair.received.empty());
    EXPECT_TRUE(pair.errors.empty());
    EXPECT_EQ(pair.a->state(), NativeTransportState::Ready);
    EXPECT_GT(pair.b->buffered_amount(), 0U) << "the sender was never held back";
    pair.hold_a = false;
    pair.a->resume_receive();
    ASSERT_TRUE(pair.queue.until([&] { return pair.received.size() == message_count; }));
    for (std::uint32_t index = 0; index < message_count; ++index)
        ASSERT_EQ(number_of(pair.received[index]), index);
    EXPECT_EQ(pair.b->buffered_amount(), 0U);
    EXPECT_TRUE(pair.errors.empty());
}

// The backend's reader waits for room in a full queue; closing must free it, not hang.
TEST(NativeReceiveBackpressure, ClosingWhileTheBackendWaitsForRoomFinishes) {
    Pair pair;
    pair.create();
    ASSERT_TRUE(pair.connect());
    pair.hold_a = true;
    send_all(pair);
    ASSERT_TRUE(pair.queue.until([&] { return pair.b->buffered_amount() > 0; }));
    run_for(pair, std::chrono::milliseconds(200));
    pair.a->close();
    pair.a.reset();
    pair.b->close();
    pair.b.reset();
    pair.queue.drain();
    EXPECT_TRUE(pair.received.empty());
}
