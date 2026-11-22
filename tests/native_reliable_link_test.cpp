#include "../src/transport/native_reliable_link.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
class Loop {
  public:
    Executor executor() {
        return [this](std::function<void()> action) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(action));
        };
    }
    void drain() {
        for (int count = 0; count < 10000; ++count) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
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

struct Pair {
    Loop loop;
    std::unique_ptr<NativeReliableLink> a;
    std::unique_ptr<NativeReliableLink> b;
    std::vector<Bytes> received;
    std::vector<std::string> errors;
    std::vector<RouteInfo> second_routes;
    bool destroy_on_receive = false;
    NativeLinkCallbacks callbacks(bool first) {
        NativeLinkCallbacks out;
        out.on_description = [this, first](rtc::Description value) {
            auto &other = first ? b : a;
            if (other) {
                other->set_remote_description(std::move(value));
            }
        };
        out.on_candidate = [this, first](rtc::Candidate value) {
            auto &other = first ? b : a;
            if (other) {
                other->add_remote_candidate(std::move(value));
            }
        };
        out.on_state = [](NativeTransportState) {};
        out.on_route = [this, first](RouteInfo route) {
            if (!first) {
                second_routes.push_back(route);
            }
        };
        out.on_error = [this](ConnectError, std::string error) {
            errors.push_back(std::move(error));
        };
        out.on_writable = [] {};
        out.on_message = [this, first](Bytes bytes) {
            if (first) {
                received.push_back(std::move(bytes));
            } else if (destroy_on_receive) {
                b.reset();
            } else {
                EXPECT_EQ(b->send(bytes), ChannelSend::Accepted);
            }
        };
        return out;
    }
    void create(std::size_t channel_maximum = 0, std::size_t second_frame_maximum = 1024) {
        auto make = [&](bool first) {
            const ReliableChannelLimits limits{
                {256U * 1024U, first ? 1024 : second_frame_maximum}, 256U * 1024U, 4, 65536, 8};
            MemoryBudget budget(4U * 1024U * 1024U);
            NativeReceiveLimits inbound{1024};
            rtc::Configuration config;
            if (channel_maximum > 0) {
                config.maxMessageSize = channel_maximum;
            }
            return std::make_unique<NativeReliableLink>(config, NativeRelayPolicy::Automatic,
                                                        limits, budget, inbound, loop.executor(),
                                                        callbacks(first));
        };
        a = make(true);
        b = make(false);
    }
    bool connect() {
        a->start_offer();
        return loop.until([this] {
            return a->state() == NativeTransportState::Ready &&
                   b->state() == NativeTransportState::Ready;
        });
    }
};

struct RawPeerPair {
    Loop loop;
    std::unique_ptr<NativeTransport> raw;
    std::unique_ptr<NativeReliableLink> link;
    std::vector<ConnectError> errors;

    NativeTransportCallbacks raw_callbacks() {
        NativeTransportCallbacks out;
        out.on_description = [this](rtc::Description value) {
            link->set_remote_description(std::move(value));
        };
        out.on_candidate = [this](rtc::Candidate value) {
            link->add_remote_candidate(std::move(value));
        };
        out.on_state = [](NativeTransportState) {};
        out.on_binary = [](Bytes) {};
        out.on_error = [this](ConnectError category, std::string) { errors.push_back(category); };
        return out;
    }
    NativeLinkCallbacks link_callbacks() {
        NativeLinkCallbacks out;
        out.on_description = [this](rtc::Description value) {
            raw->set_remote_description(std::move(value));
        };
        out.on_candidate = [this](rtc::Candidate value) {
            raw->add_remote_candidate(std::move(value));
        };
        out.on_state = [](NativeTransportState) {};
        out.on_message = [](Bytes) {};
        out.on_writable = [] {};
        out.on_error = [this](ConnectError category, std::string) { errors.push_back(category); };
        return out;
    }
    void create(std::size_t frame_maximum = 1024) {
        MemoryBudget budget(4U * 1024U * 1024U);
        const ReliableChannelLimits limits{{4096, frame_maximum}, 4096, 4, 4096, 4};
        NativeReceiveLimits inbound{4096};
        raw = std::make_unique<NativeTransport>(rtc::Configuration{}, NativeRelayPolicy::Automatic,
                                                loop.executor(), raw_callbacks());
        link = std::make_unique<NativeReliableLink>(rtc::Configuration{},
                                                    NativeRelayPolicy::Automatic, limits, budget,
                                                    inbound, loop.executor(), link_callbacks());
    }
};
} // namespace

TEST(NativeReliableLink, PreservesReliableChannelFailureCategories) {
    EXPECT_EQ(channel_failure_category(ChannelFailure::Transport), ConnectError::Transport);
    EXPECT_EQ(channel_failure_category(ChannelFailure::InvalidFrame), ConnectError::Protocol);
    EXPECT_EQ(channel_failure_category(ChannelFailure::ReceiveLimit), ConnectError::ResourceLimit);
    EXPECT_EQ(channel_failure_category(ChannelFailure::Allocation), ConnectError::ResourceLimit);
}

TEST(NativeReliableLink, WakesItsPumpAtAQuarterOfTheHighWaterMark) {
    RawPeerPair pair;
    pair.create();
    EXPECT_EQ(pair.link->writable_threshold(), 1024U);
}

TEST(NativeReliableLink, WakeupThresholdLeavesRoomForTheLargestFrame) {
    RawPeerPair nearly_full;
    nearly_full.create(4095);
    EXPECT_EQ(nearly_full.link->writable_threshold(), 1U);
    RawPeerPair full;
    full.create(4096);
    EXPECT_EQ(full.link->writable_threshold(), 0U);
}

TEST(NativeReliableLink, ReportsMalformedPeerFrameAsProtocolFailure) {
    RawPeerPair pair;
    pair.create();
    pair.raw->start_offer();
    ASSERT_TRUE(pair.loop.until([&] {
        return pair.raw->state() == NativeTransportState::Ready &&
               pair.link->state() == NativeTransportState::Ready;
    }));
    EXPECT_TRUE(pair.errors.empty());
    ASSERT_EQ(pair.raw->send(Bytes{std::byte{1}}), NativeSendResult::Accepted);
    ASSERT_TRUE(pair.loop.until([&] { return !pair.errors.empty(); }));
    EXPECT_EQ(pair.errors.front(), ConnectError::Protocol);
    EXPECT_EQ(pair.link->state(), NativeTransportState::Failed);
}

TEST(NativeReliableLink, ExchangesFragmentedBinaryAndEmptyMessages) {
    Pair pair;
    pair.create();
    ASSERT_TRUE(pair.connect());
    Bytes message(128U * 1024U);
    for (std::size_t index = 0; index < message.size(); ++index) {
        message[index] = static_cast<std::byte>(index & 255U);
    }
    EXPECT_EQ(pair.a->send(message), ChannelSend::Accepted);
    EXPECT_EQ(pair.a->send({}), ChannelSend::Accepted);
    ASSERT_TRUE(pair.loop.until([&] { return pair.received.size() == 2; }));
    EXPECT_EQ(pair.received[0], message);
    EXPECT_TRUE(pair.received[1].empty());
    EXPECT_TRUE(pair.errors.empty());
    EXPECT_EQ(pair.a->poll_route().current, RouteKind::Direct);
}

TEST(NativeReliableLink, FragmentsToNegotiatedBackendLimit) {
    Pair pair;
    pair.create(512);
    ASSERT_TRUE(pair.connect());
    const Bytes message(4096, std::byte{42});
    EXPECT_EQ(pair.a->send(message), ChannelSend::Accepted);
    ASSERT_TRUE(pair.loop.until([&] { return pair.received.size() == 1; }));
    EXPECT_EQ(pair.received[0], message);
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeReliableLink, AdaptsToPeersDifferentFrameLimits) {
    Pair pair;
    pair.create(0, 512);
    ASSERT_TRUE(pair.connect());
    const Bytes message(4096, std::byte{23});
    EXPECT_EQ(pair.a->send(message), ChannelSend::Accepted);
    ASSERT_TRUE(pair.loop.until([&] { return pair.received.size() == 1; }));
    EXPECT_EQ(pair.received[0], message);
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeReliableLink, CallbackCanDestroyLink) {
    Pair pair;
    pair.create();
    ASSERT_TRUE(pair.connect());
    pair.destroy_on_receive = true;
    EXPECT_EQ(pair.a->send(Bytes{std::byte{9}}), ChannelSend::Accepted);
    ASSERT_TRUE(pair.loop.until([&] { return !pair.b; }));
    pair.loop.drain();
    EXPECT_TRUE(pair.errors.empty());
}

TEST(NativeReliableLink, RemoteCloseReportsUnknownRoute) {
    Pair pair;
    pair.create();
    ASSERT_TRUE(pair.connect());
    pair.a->close();
    ASSERT_TRUE(pair.loop.until([&] {
        const auto state = pair.b->state();
        return state == NativeTransportState::Closed || state == NativeTransportState::Failed;
    }));
    ASSERT_FALSE(pair.second_routes.empty());
    EXPECT_EQ(pair.second_routes.back().current, RouteKind::Unknown);
    EXPECT_EQ(pair.b->poll_route().current, RouteKind::Unknown);
}

TEST(NativeReliableLink, RejectsIncompatibleLimitsAndMissingCallbacks) {
    Pair pair;
    const ReliableChannelLimits limits{{100, 64}, 100, 2, 64, 1};
    MemoryBudget budget(1024);
    NativeReceiveLimits inbound{32};
    EXPECT_THROW((NativeReliableLink{{},
                                     NativeRelayPolicy::Automatic,
                                     limits,
                                     budget,
                                     inbound,
                                     pair.loop.executor(),
                                     pair.callbacks(true)}),
                 std::invalid_argument);
    inbound.max_message_size = 64;
    rtc::Configuration config;
    config.disableFingerprintVerification = true;
    EXPECT_THROW((NativeReliableLink{config, NativeRelayPolicy::Automatic, limits, budget, inbound,
                                     pair.loop.executor(), pair.callbacks(true)}),
                 std::invalid_argument);
    EXPECT_THROW(
        (NativeReliableLink{
            {}, NativeRelayPolicy::Automatic, limits, budget, inbound, pair.loop.executor(), {}}),
        std::invalid_argument);
}
