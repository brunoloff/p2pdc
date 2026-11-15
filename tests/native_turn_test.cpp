#include "../src/transport/native_ice_config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class Queue {
  public:
    Executor executor() {
        return [this](auto task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    bool until(const std::function<bool()> &condition, std::chrono::seconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (condition())
                return true;
            std::this_thread::sleep_for(2ms);
        }
        drain();
        return condition();
    }

  private:
    void drain() {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

std::string required_env(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        throw std::invalid_argument(std::string("missing test setting: ") + name);
    return value;
}

IceConfig turn_config(bool valid_credentials) {
    IceConfig config;
    config.relay_policy = RelayPolicy::RequireLocalRelay;
    config.servers = {TurnServer{
        required_env("P2PDC_TEST_TURN_HOST"), 34789, required_env("P2PDC_TEST_TURN_USER"),
        valid_credentials ? required_env("P2PDC_TEST_TURN_PASSWORD") : "deliberately-wrong",
        TurnTransport::Tcp}};
    return config;
}

struct Pair {
    Queue queue;
    std::unique_ptr<NativeTransport> first;
    std::unique_ptr<NativeTransport> second;
    std::vector<Bytes> received;
    std::vector<std::string> errors;
    bool first_was_ready = false;
    bool second_was_ready = false;

    NativeTransportCallbacks callbacks(bool is_first) {
        NativeTransportCallbacks out;
        out.on_description = [this, is_first](rtc::Description value) {
            auto &peer = is_first ? second : first;
            if (peer)
                peer->set_remote_description(std::move(value));
        };
        out.on_candidate = [this, is_first](rtc::Candidate value) {
            auto &peer = is_first ? second : first;
            if (peer)
                peer->add_remote_candidate(std::move(value));
        };
        out.on_state = [this, is_first](NativeTransportState value) {
            if (value == NativeTransportState::Ready) {
                (is_first ? first_was_ready : second_was_ready) = true;
            }
        };
        out.on_error = [this](ConnectError, std::string value) {
            errors.push_back(std::move(value));
        };
        out.on_binary = [this, is_first](Bytes value) {
            if (is_first)
                received.push_back(std::move(value));
            else
                EXPECT_EQ(second->send(value), NativeSendResult::Accepted);
        };
        return out;
    }
    void create_with(const IceConfig &ice) {
        auto config = make_native_ice_config(ice);
        first = std::make_unique<NativeTransport>(config.rtc, config.policy, queue.executor(),
                                                  callbacks(true));
        second = std::make_unique<NativeTransport>(config.rtc, config.policy, queue.executor(),
                                                   callbacks(false));
        first->start_offer();
    }
    void create(bool valid_credentials) { create_with(turn_config(valid_credentials)); }
    bool ready() const {
        return first->state() == NativeTransportState::Ready &&
               second->state() == NativeTransportState::Ready;
    }
};
} // namespace

TEST(NativeTurnControlled, TcpRelayCarriesBinaryAndReportsBothSelectedRelays) {
    Pair pair;
    pair.create(true);
    ASSERT_TRUE(pair.queue.until([&] { return pair.ready() || !pair.errors.empty(); }, 20s));
    ASSERT_TRUE(pair.errors.empty());
    ASSERT_TRUE(pair.ready());
    const auto route = pair.first->poll_route();
    ASSERT_TRUE(route.selected_pair);
    EXPECT_EQ(route.current, RouteKind::TurnRelayed);
    EXPECT_EQ(route.turn_history, TurnHistory::Used);
    EXPECT_EQ(route.selected_pair->local, CandidateKind::Relayed);
    EXPECT_EQ(route.selected_pair->remote, CandidateKind::Relayed);
    const Bytes payload{std::byte{0}, std::byte{255}, std::byte{42}};
    ASSERT_EQ(pair.first->send(payload), NativeSendResult::Accepted);
    ASSERT_TRUE(pair.queue.until([&] { return !pair.received.empty(); }, 10s));
    EXPECT_EQ(pair.received.front(), payload);
}

#ifdef P2PDC_TEST_BLOCKED_DIRECT
TEST(NativeTurnFallback, DirectUdpBlockedWithoutTurnCannotConnect) {
    Pair pair;
    pair.create_with({});
    EXPECT_FALSE(pair.queue.until([&] { return pair.ready(); }, 8s));
    EXPECT_FALSE(pair.first_was_ready);
    EXPECT_FALSE(pair.second_was_ready);
    EXPECT_EQ(pair.first->send(Bytes{std::byte{1}}), NativeSendResult::NotReady);
}

TEST(NativeTurnFallback, AutomaticPeersUseTurnWhenDirectUdpIsBlocked) {
    Pair pair;
    auto config = turn_config(true);
    config.relay_policy = RelayPolicy::Automatic;
    pair.create_with(config);
    ASSERT_TRUE(pair.queue.until([&] { return pair.ready() || !pair.errors.empty(); }, 20s));
    ASSERT_TRUE(pair.errors.empty());
    ASSERT_TRUE(pair.ready());
    const auto first = pair.first->poll_route(), second = pair.second->poll_route();
    ASSERT_TRUE(first.selected_pair);
    ASSERT_TRUE(second.selected_pair);
    EXPECT_EQ(first.current, RouteKind::TurnRelayed);
    EXPECT_EQ(second.current, RouteKind::TurnRelayed);
    EXPECT_EQ(first.turn_history, TurnHistory::Used);
    EXPECT_EQ(second.turn_history, TurnHistory::Used);
    EXPECT_TRUE(first.selected_pair->local == CandidateKind::Relayed ||
                first.selected_pair->remote == CandidateKind::Relayed);
    const Bytes payload{std::byte{0}, std::byte{255}, std::byte{42}};
    ASSERT_EQ(pair.first->send(payload), NativeSendResult::Accepted);
    ASSERT_TRUE(pair.queue.until([&] { return !pair.received.empty(); }, 10s));
    EXPECT_EQ(pair.received.front(), payload);
}

TEST(NativeTurnFallback, AutomaticPeersCannotUseTurnWithWrongCredentials) {
    Pair pair;
    auto config = turn_config(false);
    config.relay_policy = RelayPolicy::Automatic;
    pair.create_with(config);
    EXPECT_FALSE(pair.queue.until([&] { return pair.ready(); }, 8s));
    EXPECT_FALSE(pair.first_was_ready);
    EXPECT_FALSE(pair.second_was_ready);
    EXPECT_NE(pair.first->poll_route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(pair.first->send(Bytes{std::byte{1}}), NativeSendResult::NotReady);
}
#endif

TEST(NativeTurnControlled, WrongCredentialsDoNotProduceReadyRelay) {
    Pair pair;
    pair.create(false);
    EXPECT_FALSE(pair.queue.until([&] { return pair.ready(); }, 8s));
    EXPECT_FALSE(pair.first_was_ready);
    EXPECT_FALSE(pair.second_was_ready);
    EXPECT_NE(pair.first->poll_route().current, RouteKind::TurnRelayed);
    EXPECT_EQ(pair.first->send(Bytes{std::byte{1}}), NativeSendResult::NotReady);
    EXPECT_EQ(pair.second->send(Bytes{std::byte{1}}), NativeSendResult::NotReady);
}
