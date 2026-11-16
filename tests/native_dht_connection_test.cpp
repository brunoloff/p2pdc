#include "../src/discovery/native_signal_pipe.hpp"
#include "../src/discovery/native_store.hpp"
#include "../src/security/handshake_gate.hpp"
#include "../src/transport/native_signaling.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class Loop {
  public:
    Executor executor() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    bool until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        drain();
        return done();
    }

  private:
    void drain() {
        for (int index = 0; index < 1000; ++index) {
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
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

struct GateTransport : HandshakeTransport {
    NativeReliableLink *link = nullptr;
    ChannelSend send(ByteView message) override { return link->send(message); }
    void limit_receive(std::size_t bytes) override { link->set_receive_limit(bytes); }
    void close() override { link->close(); }
};

struct Peer {
    Identity identity;
    RecipientKeyPair box;
    SignalingRecipient recipient;
    std::array<std::byte, 20> inbox;
    HandshakeRole role;
    std::shared_ptr<dht::DhtRunner> runner;
    Loop &loop;
    Bytes capability = Bytes(32, std::byte{8});
    SignalingPolicy policy{100, 60, 5, 32768};
    std::unique_ptr<NativeReliableLink> link;
    std::unique_ptr<NativeSignaling> signaling;
    std::unique_ptr<NativeSignalPipe> pipe;
    GateTransport gate_transport;
    std::unique_ptr<HandshakeGate> gate;
    std::vector<Bytes> messages;
    int failures = 0;
    int candidates = 0;
    int ignored_invalid = 0;
    bool ready = false;

    Peer(std::byte seed, HandshakeRole peer_role, std::shared_ptr<dht::DhtRunner> node,
         Loop &events)
        : identity(std::get<Identity>(Identity::import_seed(Bytes(32, seed)))),
          box(std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, seed)))),
          recipient{identity.public_key(), {seed}}, inbox{seed}, role(peer_role),
          runner(std::move(node)), loop(events) {}
    ~Peer() { close(); }
    void close() {
        if (pipe)
            pipe->close();
        if (gate)
            gate->close();
        if (signaling)
            signaling->close();
        if (link)
            link->close();
    }
    void on_ready() {
        if (gate) {
            gate->set_transport_ready(true);
            return;
        }
        auto context = signaling->handshake_context();
        if (!context) {
            ++failures;
            return;
        }
        gate = std::make_unique<HandshakeGate>(
            identity, *context, role, 1024, 4096, gate_transport,
            HandshakeGateCallbacks{[this](std::uint32_t) { ready = true; },
                                   [this](Bytes value) { messages.push_back(std::move(value)); },
                                   [] {}, [this](GateError) { ++failures; }});
        gate->set_transport_ready(true);
    }
    NativeLinkCallbacks link_callbacks() {
        NativeLinkCallbacks callbacks;
        callbacks.on_description = [this](rtc::Description value) {
            if (signaling->local_description(identity, capability, std::move(value), 100, 150,
                                             policy) != FlowResult::Accepted)
                ++failures;
        };
        callbacks.on_candidate = [this](rtc::Candidate value) {
            if (signaling->local_candidate(identity, capability, std::move(value), 100, 150,
                                           policy) != FlowResult::Accepted)
                ++failures;
            ++candidates;
        };
        callbacks.on_state = [this](NativeTransportState value) {
            if (value == NativeTransportState::Ready)
                on_ready();
            if (value == NativeTransportState::Suspended && gate)
                gate->set_transport_ready(false);
            if (value == NativeTransportState::Failed)
                ++failures;
        };
        callbacks.on_message = [this](Bytes value) {
            if (gate)
                gate->on_message(value);
            else
                ++failures;
        };
        callbacks.on_writable = [this] {
            if (gate)
                gate->on_writable();
        };
        callbacks.on_error = [this](ConnectError, std::string) { ++failures; };
        return callbacks;
    }
    void create_link() {
        MemoryBudget budget(65536);
        ReliableChannelLimits limits{{4096, 1024}, 4096, 8, 4096, 4};
        link = std::make_unique<NativeReliableLink>(
            rtc::Configuration{}, NativeRelayPolicy::Automatic, limits, budget,
            NativeReceiveLimits{budget, 8192, 64, 1024}, loop.executor(), link_callbacks());
        gate_transport.link = link.get();
    }
    void create_pipe(const Peer &remote) {
        NativeStore store(runner, {32768, 128, 1048576});
        auto result = NativeSignalPipe::create(
            store, inbox, remote.inbox, MemoryBudget(2097152),
            {{524288, 128, 32768}, {524288, 64, 32768}}, loop.executor(),
            SignalPipeCallbacks{
                [this](Bytes value) {
                    const bool invalid = value == Bytes{std::byte{255}};
                    const auto result = signaling->receive(value, box, capability, policy);
                    if (invalid && result == FlowResult::Ignored)
                        ++ignored_invalid;
                    if (result == FlowResult::Failed || result == FlowResult::Closed)
                        ++failures;
                },
                [this](MailboxError, std::optional<StoreError>) { ++failures; },
                [this](OutboxError) { ++failures; }, [] {},
                [this](bool success) {
                    if (!success)
                        ++failures;
                }});
        pipe = std::move(std::get<std::unique_ptr<NativeSignalPipe>>(result));
    }
    void create_signaling(const Peer &remote) {
        NativeSignalBinding binding{recipient,
                                    remote.recipient,
                                    box.public_key(),
                                    remote.box.public_key(),
                                    AttemptId{std::byte{7}},
                                    role};
        signaling = std::make_unique<NativeSignaling>(
            binding, capability, *link, CandidateLimits{4096, 256, 4361}, MemoryBudget(65536),
            65536, 64,
            [this](Bytes value) {
                return pipe->send(std::move(value)) == SignalingQueueResult::Accepted;
            },
            [](SignalingKind) { return false; });
    }
};

class DhtConnectionTest : public ::testing::Test {
  protected:
    Loop loop;
    std::shared_ptr<dht::DhtRunner> a = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> b = std::make_shared<dht::DhtRunner>();
    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345682;
        config.peer_discovery = false;
        config.peer_publish = false;
        a->run("127.0.0.1", "::1", "0", config);
        b->run("127.0.0.1", "::1", "0", config);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        b->bootstrap(a->getBound(), [done](bool success) { done->set_value(success); });
        ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(future.get());
    }
    void TearDown() override {
        a->join();
        b->join();
    }
};
} // namespace

TEST_F(DhtConnectionTest, AuthenticatesAndExchangesMessageThroughDhtSignaling) {
    Peer first(std::byte{1}, HandshakeRole::Offerer, a, loop);
    Peer second(std::byte{2}, HandshakeRole::Answerer, b, loop);
    first.create_link();
    second.create_link();
    first.create_pipe(second);
    second.create_pipe(first);
    first.create_signaling(second);
    second.create_signaling(first);
    ASSERT_EQ(first.pipe->registration().wait_for(10s), std::future_status::ready);
    ASSERT_EQ(second.pipe->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(first.pipe->registration().get(), 0U);
    ASSERT_NE(second.pipe->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(first.pipe->catch_up()));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(second.pipe->catch_up()));
    ASSERT_EQ(first.pipe->send(Bytes{std::byte{255}}), SignalingQueueResult::Accepted);
    first.link->start_offer();
    ASSERT_TRUE(loop.until(
        [&] { return (first.ready && second.ready) || first.failures || second.failures; }));
    ASSERT_EQ(first.failures, 0);
    ASSERT_EQ(second.failures, 0);
    ASSERT_TRUE(loop.until([&] { return second.ignored_invalid > 0; }));
    ASSERT_GT(first.candidates + second.candidates, 0);
    ASSERT_EQ(first.gate->send(Bytes{std::byte{0}, std::byte{255}}), ChannelSend::Accepted);
    ASSERT_TRUE(loop.until([&] { return !second.messages.empty(); }));
    EXPECT_EQ(second.messages.front(), (Bytes{std::byte{0}, std::byte{255}}));
    EXPECT_EQ(first.failures, 0);
    EXPECT_EQ(second.failures, 0);
    first.pipe->close();
    EXPECT_FALSE(first.pipe->is_open());
    EXPECT_EQ(first.pipe->send(Bytes{std::byte{1}}), SignalingQueueResult::Closed);
    EXPECT_EQ(std::get<CatchupError>(first.pipe->catch_up()), CatchupError::Closed);
}

TEST(NativeSignalPipeValidation, RejectsInvalidKeysAndStoppedRunner) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, {1024, 16, 16384});
    std::array<std::byte, 20> valid{std::byte{1}};
    std::array<std::byte, 20> invalid{};
    SignalPipeLimits limits{{1024, 4, 512}, {1024, 4, 512}};
    SignalPipeCallbacks callbacks{[](Bytes) {}, [](MailboxError, std::optional<StoreError>) {},
                                  [](OutboxError) {}, [] {}, [](bool) {}};
    EXPECT_THROW((NativeSignalPipe::create(store, valid, valid, MemoryBudget(4096), limits,
                                           [](auto) {}, {})),
                 std::invalid_argument);
    EXPECT_EQ(std::get<StoreError>(NativeSignalPipe::create(
                  store, valid, invalid, MemoryBudget(4096), limits, [](auto) {}, callbacks)),
              StoreError::InvalidKey);
    EXPECT_EQ(std::get<StoreError>(NativeSignalPipe::create(
                  store, valid, valid, MemoryBudget(4096), limits, [](auto) {}, callbacks)),
              StoreError::NotRunning);
}
