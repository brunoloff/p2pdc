#include "../src/protocol/reliable_pump.hpp"
#include "../src/transport/native_backend_features.hpp"
#include "../src/transport/native_transport.hpp"
#include "p2pdc/reassembler.h"

#include <gtest/gtest.h>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
class Queue {
  public:
    Executor executor() {
        return [this](std::function<void()> action) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(action));
        };
    }
    void drain() {
        for (int count = 0; count < 10000; ++count) {
            std::function<void()> action;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty()) {
                    return;
                }
                action = std::move(tasks_.front());
                tasks_.pop_front();
            }
            action();
        }
    }
    bool until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        drain();
        return done();
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

class NativeSink : public ReliableSink {
  public:
    explicit NativeSink(NativeTransport &transport) : transport_(transport) {}
    std::size_t buffered_amount() const override { return transport_.buffered_amount(); }
    SinkResult send(ByteView frame) override {
        switch (transport_.send(frame)) {
        case NativeSendResult::Accepted:
            return SinkResult::Accepted;
        case NativeSendResult::NotReady:
            return SinkResult::NotReady;
        default:
            return SinkResult::Failed;
        }
    }

  private:
    NativeTransport &transport_;
};

struct Pair {
    Queue queue;
    std::unique_ptr<NativeTransport> a;
    std::unique_ptr<NativeTransport> b;
    std::vector<Bytes> received;
    std::vector<std::string> errors;
    std::vector<ConnectError> error_categories;
    std::vector<NativeTransportState> states;
    int notifications = 0;

    NativeTransportCallbacks callbacks(bool first) {
        NativeTransportCallbacks result;
        result.on_description = [this, first](rtc::Description description) {
            ++notifications;
            auto &other = first ? b : a;
            if (other && other->state() != NativeTransportState::Closed) {
                other->set_remote_description(std::move(description));
            }
        };
        result.on_candidate = [this, first](rtc::Candidate candidate) {
            ++notifications;
            auto &other = first ? b : a;
            if (other && other->state() != NativeTransportState::Closed) {
                other->add_remote_candidate(std::move(candidate));
            }
        };
        result.on_state = [this, first](NativeTransportState state) {
            ++notifications;
            if (!first) {
                states.push_back(state);
            }
        };
        result.on_error = [this](ConnectError category, std::string error) {
            error_categories.push_back(category);
            errors.push_back(std::move(error));
        };
        result.on_binary = [this, first](Bytes bytes) {
            if (first) {
                received.push_back(std::move(bytes));
            } else {
                EXPECT_EQ(b->send(bytes), NativeSendResult::Accepted);
            }
        };
        return result;
    }
    void create(rtc::Configuration config = {},
                NativeRelayPolicy second = NativeRelayPolicy::Automatic,
                NativeReceiveLimits limits = {}) {
        a = std::make_unique<NativeTransport>(config, NativeRelayPolicy::Automatic,
                                              queue.executor(), callbacks(true), limits);
        b = std::make_unique<NativeTransport>(config, second, queue.executor(), callbacks(false),
                                              std::move(limits));
    }
    bool connect() {
        a->start_offer();
        return queue.until([this] {
            return a->state() == NativeTransportState::Ready &&
                   b->state() == NativeTransportState::Ready;
        });
    }
};
} // namespace

TEST(NativeBackendFeatures, RequiresAdmissionAndRelayCapabilities) {
    EXPECT_FALSE(supports_native_backend(0, false));
    EXPECT_FALSE(supports_native_backend(native_send_admission, false));
    EXPECT_TRUE(supports_native_backend(native_send_admission | native_safe_ice_detach, false));
    EXPECT_FALSE(supports_native_backend(native_send_admission, true));
    EXPECT_FALSE(supports_native_backend(native_force_relay, true));
    EXPECT_FALSE(supports_native_backend(native_send_admission | native_force_relay, true));
    EXPECT_TRUE(supports_native_backend(
        native_send_admission | native_force_relay | native_safe_ice_detach, true));
    EXPECT_TRUE(supports_native_backend(~0U, true));
}

TEST(NativeTransport, RequiresExecutorAndEssentialCallbacks) {
    Pair pair;
    EXPECT_THROW(NativeTransport({}, NativeRelayPolicy::Automatic, {}, pair.callbacks(true)),
                 std::invalid_argument);
    EXPECT_THROW(NativeTransport({}, NativeRelayPolicy::Automatic, pair.queue.executor(), {}),
                 std::invalid_argument);
    // Deliberate invalid input exercises module-boundary validation.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_THROW(NativeTransport({}, static_cast<NativeRelayPolicy>(99), pair.queue.executor(),
                                 pair.callbacks(true)),
                 std::invalid_argument);
    EXPECT_THROW(NativeTransport({}, NativeRelayPolicy::RequireLocalRelay, pair.queue.executor(),
                                 pair.callbacks(true)),
                 std::invalid_argument);
}

TEST(NativeTransport, LiveDirectEchoPreservesEmptyBinaryAndOrder) {
    Pair pair;
    pair.create();
    ASSERT_TRUE(pair.connect());
    EXPECT_TRUE(pair.errors.empty());
    const std::vector<Bytes> messages{
        {}, {std::byte{0}, std::byte{255}, std::byte{17}}, {std::byte{2}}};
    for (const auto &bytes : messages) {
        EXPECT_EQ(pair.a->send(bytes), NativeSendResult::Accepted);
    }
    ASSERT_TRUE(pair.queue.until([&] { return pair.received.size() == messages.size(); }));
    EXPECT_EQ(pair.received, messages);
    EXPECT_GT(pair.a->max_message_size(), 0);
    const auto route = pair.a->poll_route();
    EXPECT_EQ(route.current, RouteKind::Direct);
    if (route.selected_pair) {
        EXPECT_NE(route.selected_pair->local, CandidateKind::Unknown);
    } else {
        FAIL() << "Direct route lacks selected-pair evidence";
    }
    EXPECT_EQ(route.turn_history, TurnHistory::Unknown);
    EXPECT_TRUE(route.observed_at);
    EXPECT_THROW(
        pair.a->set_remote_description(rtc::Description("", rtc::Description::Type::Answer)),
        std::logic_error);
    pair.a->close();
    EXPECT_EQ(pair.a->send({}), NativeSendResult::NotReady);
    EXPECT_EQ(pair.a->poll_route().current, RouteKind::Unknown);
}

TEST(NativeTransport, AnnouncesReadyBeforeDeliveringImmediateData) {
    Pair pair;
    bool ready_notified = false;
    bool delivered = false;
    auto first = pair.callbacks(true);
    first.on_state = [&](NativeTransportState state) {
        if (state == NativeTransportState::Ready) {
            EXPECT_EQ(pair.a->send(Bytes{std::byte{7}}), NativeSendResult::Accepted);
        }
    };
    auto second = pair.callbacks(false);
    second.on_state = [&](NativeTransportState state) {
        if (state == NativeTransportState::Ready) {
            ready_notified = true;
        }
    };
    second.on_binary = [&](Bytes bytes) {
        EXPECT_TRUE(ready_notified);
        EXPECT_EQ(bytes, Bytes{std::byte{7}});
        delivered = true;
    };
    pair.a = std::make_unique<NativeTransport>(rtc::Configuration{}, NativeRelayPolicy::Automatic,
                                               pair.queue.executor(), std::move(first));
    pair.b = std::make_unique<NativeTransport>(rtc::Configuration{}, NativeRelayPolicy::Automatic,
                                               pair.queue.executor(), std::move(second));
    pair.a->start_offer();
    ASSERT_TRUE(pair.queue.until([&] { return delivered; }));
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeTransport, PumpDeliversFragmentedMessagesThroughLiveChannel) {
    Pair pair;
    pair.create();
    ASSERT_TRUE(pair.connect());
    MemoryBudget budget(2048);
    const FrameLimits limits{128, 24};
    ReliableSender sender(limits, budget, 128, 2);
    NativeSink sink(*pair.a);
    ReliablePump pump(sender, sink, 1024, 2);
    const Bytes message(23, std::byte{42});
    ASSERT_EQ(sender.enqueue(message), SenderEnqueue::Accepted);
    ASSERT_EQ(sender.enqueue({}), SenderEnqueue::Accepted);
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(pump.run(), PumpResult::More);
    }
    EXPECT_EQ(pump.run(), PumpResult::Idle);
    ASSERT_TRUE(pair.queue.until([&] { return pair.received.size() == 6; }));
    Reassembler receiver(limits);
    std::vector<Bytes> messages;
    for (const auto &frame : pair.received) {
        auto result = receiver.push(frame);
        ASSERT_FALSE(result.error);
        if (result.message) {
            messages.push_back(std::move(*result.message));
        }
    }
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0], message);
    EXPECT_TRUE(messages[1].empty());
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeTransport, RejectsNotReadyAndOversizedWithoutPartialSend) {
    Pair pair;
    rtc::Configuration config;
    config.maxMessageSize = 1024;
    pair.create(config);
    EXPECT_EQ(pair.a->send({}), NativeSendResult::NotReady);
    EXPECT_EQ(pair.a->buffered_amount(), 0);
    EXPECT_EQ(pair.a->max_message_size(), 0);
    ASSERT_TRUE(pair.connect());
    const auto maximum = pair.a->max_message_size();
    ASSERT_LE(maximum, 1024);
    EXPECT_EQ(pair.a->send(Bytes(maximum + 1)), NativeSendResult::TooLarge);
    EXPECT_EQ(pair.a->buffered_amount(), 0);
    EXPECT_TRUE(pair.received.empty());
    EXPECT_EQ(pair.a->send(Bytes(maximum, std::byte{7})), NativeSendResult::Accepted);
    ASSERT_TRUE(pair.queue.until([&] { return pair.received.size() == 1; }));
    EXPECT_EQ(pair.received.front(), Bytes(maximum, std::byte{7}));
}

TEST(NativeTransport, ReceiveLimitFailsConnectionAndReleasesBudget) {
    Pair pair;
    MemoryBudget budget(8);
    pair.create({}, NativeRelayPolicy::Automatic, {budget, 8, 2, 1});
    ASSERT_TRUE(pair.connect());
    ASSERT_EQ(pair.a->send(Bytes{std::byte{1}, std::byte{2}}), NativeSendResult::Accepted);
    ASSERT_TRUE(pair.queue.until([&] { return pair.b->state() == NativeTransportState::Failed; }));
    EXPECT_TRUE(pair.received.empty());
    EXPECT_EQ(budget.used(), 0U);
    ASSERT_FALSE(pair.errors.empty());
    EXPECT_NE(pair.errors.front().find("receive limits"), std::string::npos);
    ASSERT_FALSE(pair.error_categories.empty());
    EXPECT_EQ(pair.error_categories.front(), ConnectError::ResourceLimit);
}

TEST(NativeTransport, RejectsInvalidReceiveLimits) {
    Pair pair;
    EXPECT_THROW(NativeTransport({}, NativeRelayPolicy::Automatic, pair.queue.executor(),
                                 pair.callbacks(true), {MemoryBudget(4), 4, 0, 4}),
                 std::invalid_argument);
}

TEST(NativeTransport, RejectsDuplicateOfferAndCallsAfterClose) {
    Pair pair;
    pair.create();
    pair.a->start_offer();
    EXPECT_THROW(pair.a->start_offer(), std::logic_error);
    pair.a->close();
    EXPECT_THROW(pair.a->start_offer(), std::logic_error);
    EXPECT_THROW(pair.a->add_remote_candidate(rtc::Candidate{}), std::logic_error);
    EXPECT_THROW(
        pair.a->set_remote_description(rtc::Description{"", rtc::Description::Type::Offer}),
        std::logic_error);
    pair.queue.drain();
    EXPECT_EQ(pair.notifications, 0);
}

TEST(NativeTransport, DestructionSuppressesQueuedBackendCallbacks) {
    Pair pair;
    pair.create();
    pair.a->start_offer();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pair.a.reset();
    pair.b.reset();
    pair.queue.drain();
    EXPECT_EQ(pair.notifications, 0);
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeTransport, UserCallbackMayDestroyAdapter) {
    Pair pair;
    auto callbacks = pair.callbacks(true);
    callbacks.on_description = [&pair](rtc::Description) { pair.a.reset(); };
    pair.a = std::make_unique<NativeTransport>(rtc::Configuration{}, NativeRelayPolicy::Automatic,
                                               pair.queue.executor(), std::move(callbacks));
    pair.a->start_offer();
    ASSERT_TRUE(pair.queue.until([&] { return !pair.a; }));
    pair.queue.drain();
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeTransport, RelayPolicyNeverReadiesAnUnverifiedLocalCandidate) {
    Pair pair;
    rtc::Configuration config;
    config.iceServers.emplace_back("127.0.0.1", 9, "unreachable", "unreachable");
    pair.create(config, NativeRelayPolicy::RequireLocalRelay);
    pair.a->start_offer();
    EXPECT_FALSE(pair.queue.until([&] { return pair.b->state() == NativeTransportState::Ready; }));
    EXPECT_EQ(pair.b->send({}), NativeSendResult::NotReady);
    // The receiving peer's Ready is forbidden even if the direct peer becomes Ready.
    EXPECT_EQ(std::find(pair.states.begin(), pair.states.end(), NativeTransportState::Ready),
              pair.states.end());
    EXPECT_EQ(pair.b->poll_route().current, RouteKind::Unknown);
}

TEST(NativeTransport, RepeatedBufferedSendsRemainAcceptedAndOrdered) {
    Pair pair;
    rtc::Configuration config;
    config.maxMessageSize = 65536;
    constexpr std::size_t receive_budget = 64U * 1024U * 1024U;
    pair.create(config, NativeRelayPolicy::Automatic,
                {MemoryBudget(receive_budget), receive_budget, 1024, 65536});
    ASSERT_TRUE(pair.connect());
    constexpr std::size_t count = 512;
    constexpr std::size_t message_size = 65536;
    bool saw_buffering = false;
    for (std::size_t index = 0; index < count; ++index) {
        Bytes message(message_size, std::byte{0xff});
        message[0] = static_cast<std::byte>(index & 255);
        message[1] = static_cast<std::byte>(index >> 8);
        const auto result = pair.a->send(message);
        if (result != NativeSendResult::Accepted) {
            pair.queue.drain();
        }
        ASSERT_EQ(result, NativeSendResult::Accepted)
            << (pair.errors.empty() ? "no backend diagnostic" : pair.errors.front());
        saw_buffering = saw_buffering || pair.a->buffered_amount() > 0;
    }
    ASSERT_TRUE(pair.queue.until([&] { return pair.received.size() == count; }));
    EXPECT_TRUE(saw_buffering);
    EXPECT_TRUE(pair.errors.empty());
    for (std::size_t index = 0; index < count; ++index) {
        ASSERT_EQ(pair.received[index].size(), message_size);
        EXPECT_EQ(pair.received[index][0], static_cast<std::byte>(index & 255));
        EXPECT_EQ(pair.received[index][1], static_cast<std::byte>(index >> 8));
    }
}

TEST(NativeTransport, WritableThresholdIsStoredAndAppliedToTheChannel) {
    Pair pair;
    pair.create();
    EXPECT_EQ(pair.a->writable_threshold(), 0U);
    pair.a->set_writable_threshold(4096);
    ASSERT_TRUE(pair.connect());
    EXPECT_EQ(pair.a->writable_threshold(), 4096U);
    pair.a->set_writable_threshold(8192);
    EXPECT_EQ(pair.a->writable_threshold(), 8192U);
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeTransport, RejectsUnsupportedDescriptionType) {
    Pair pair;
    pair.create();
    EXPECT_THROW(
        pair.a->set_remote_description(rtc::Description("", rtc::Description::Type::Pranswer)),
        std::invalid_argument);
    EXPECT_EQ(pair.a->state(), NativeTransportState::Connecting);
}
