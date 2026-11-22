#include "../src/transport/native_attempt.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class AttemptLoop {
  public:
    Executor executor() {
        return [this](auto task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    bool until(const std::function<bool()> &condition) {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
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

std::string setting(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        throw std::invalid_argument(std::string("missing test setting: ") + name);
    return value;
}

IceConfig relay_config() {
    IceConfig result;
    result.relay_policy = RelayPolicy::RequireLocalRelay;
    result.servers = {TurnServer{setting("P2PDC_TEST_TURN_HOST"), 34789,
                                 setting("P2PDC_TEST_TURN_USER"),
                                 setting("P2PDC_TEST_TURN_PASSWORD"), TurnTransport::Tcp}};
    return result;
}

struct Peer {
    std::shared_ptr<Identity> identity;
    std::shared_ptr<RecipientKeyPair> box;
    SignalingRecipient recipient;
    std::array<std::byte, 20> address;
    std::shared_ptr<Bytes> capability = std::make_shared<Bytes>(32, std::byte{8});
    std::unique_ptr<NativeAttempt> attempt;
    std::vector<Bytes> messages;
    std::vector<RouteInfo> routes;
    int ready = 0;
    int failed = 0;

    explicit Peer(std::byte seed)
        : identity(std::make_shared<Identity>(
              std::get<Identity>(Identity::import_seed(Bytes(32, seed))))),
          box(std::make_shared<RecipientKeyPair>(
              std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, seed))))),
          recipient{identity->public_key(), {seed}}, address{seed} {}
    NativeAttemptCallbacks callbacks() {
        return {[this] { ++ready; }, [this](Bytes value) { messages.push_back(std::move(value)); },
                [] {}, [this](RouteInfo value) { routes.push_back(value); }, [this] { ++failed; }};
    }
    NativeAttemptConfig config(Peer &remote, std::shared_ptr<dht::DhtRunner> runner,
                               HandshakeRole role) {
        MemoryBudget budget(4194304);
        return {NativeStore(std::move(runner), {32768, 128, 1048576}),
                address,
                remote.address,
                {recipient, remote.recipient, box->public_key(), remote.box->public_key(),
                 AttemptId{std::byte{7}}, role},
                identity,
                box,
                capability,
                relay_config(),
                budget,
                {{524288, 128, 32768}, {524288, 64, 32768}},
                {{4096, 1024}, 4096, 8, 4096, 4},
                NativeReceiveLimits{1024},
                {4096, 256, 4361},
                65536,
                64,
                1024,
                4096,
                {100, 60, 5, 32768},
                50,
                [] { return std::uint64_t{100}; }};
    }
};

class NativeTurnAttemptTest : public ::testing::Test {
  protected:
    AttemptLoop loop;
    std::shared_ptr<dht::DhtRunner> first = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> second = std::make_shared<dht::DhtRunner>();
    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345689;
        config.peer_discovery = false;
        config.peer_publish = false;
        first->run("127.0.0.1", "::1", "0", config);
        second->run("127.0.0.1", "::1", "0", config);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        second->bootstrap(first->getBound(), [done](bool ok) { done->set_value(ok); });
        ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(future.get());
    }
    void TearDown() override {
        first->join();
        second->join();
    }
};
} // namespace

TEST_F(NativeTurnAttemptTest, AuthenticatedDhtSignalingAndBinaryExchangeUseRelay) {
    Peer a(std::byte{1}), b(std::byte{2});
    a.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(NativeAttempt::create(
        a.config(b, first, HandshakeRole::Offerer), loop.executor(), a.callbacks())));
    b.attempt = std::move(std::get<std::unique_ptr<NativeAttempt>>(NativeAttempt::create(
        b.config(a, second, HandshakeRole::Answerer), loop.executor(), b.callbacks())));
    bool started_a = false, started_b = false;
    ASSERT_TRUE(loop.until([&] {
        if (!started_b)
            started_b = b.attempt->poll_start() == NativeStart::Started;
        if (!started_a)
            started_a = a.attempt->poll_start() == NativeStart::Started;
        return (a.ready && b.ready && !a.routes.empty()) || a.failed || b.failed;
    }));
    ASSERT_EQ(a.failed, 0);
    ASSERT_EQ(b.failed, 0);
    ASSERT_EQ(a.ready, 1);
    ASSERT_EQ(b.ready, 1);
    ASSERT_FALSE(a.routes.empty());
    EXPECT_EQ(a.routes.back().current, RouteKind::TurnRelayed);
    EXPECT_EQ(a.routes.back().turn_history, TurnHistory::Used);
    const auto route = a.attempt->poll_route();
    ASSERT_TRUE(route.selected_pair);
    EXPECT_EQ(route.current, RouteKind::TurnRelayed);
    EXPECT_EQ(route.turn_history, TurnHistory::Used);
    EXPECT_EQ(route.selected_pair->local, CandidateKind::Relayed);
    EXPECT_EQ(route.selected_pair->remote, CandidateKind::Relayed);
    const Bytes payload{std::byte{0}, std::byte{255}, std::byte{42}};
    ASSERT_EQ(a.attempt->send(payload), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !b.messages.empty(); }));
    EXPECT_EQ(b.messages.front(), payload);
    a.attempt->close();
    b.attempt->close();
}
