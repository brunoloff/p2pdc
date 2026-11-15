#include "../src/core/shutdown_coordinator.hpp"
#include "../src/discovery/native_mailbox.hpp"
#include "../src/discovery/native_publisher.hpp"
#include "../src/discovery/native_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
constexpr std::array<std::byte, 20> address{std::byte{23}};
constexpr StoreLimits limits{256, 8, 2048};

class Loop {
  public:
    Executor executor() {
        return [this](auto task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
            ++submitted_;
        };
    }
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
    bool until(const std::function<bool()> &condition) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (condition())
                return true;
            std::this_thread::sleep_for(2ms);
        }
        drain();
        return condition();
    }
    unsigned submitted() const { return submitted_.load(); }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    std::atomic<unsigned> submitted_{0};
};

class NativeShutdownTest : public ::testing::Test {
  protected:
    std::shared_ptr<dht::DhtRunner> writer = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> reader = std::make_shared<dht::DhtRunner>();
    Loop loop;
    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345688;
        config.peer_discovery = false;
        config.peer_publish = false;
        writer->run("127.0.0.1", "::1", "0", config);
        reader->run("127.0.0.1", "::1", "0", config);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        reader->bootstrap(writer->getBound(), [done](bool ok) { done->set_value(ok); });
        ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(future.get());
    }
    void TearDown() override {
        writer->join();
        reader->join();
    }
    std::shared_ptr<NativeMailbox> make_mailbox(MailboxCallbacks handlers) {
        auto result = NativeMailbox::create(NativeStore(reader, limits), address, {2048, 8, 256},
                                            MemoryBudget(2048), loop.executor(), handlers);
        if (auto *error = std::get_if<StoreError>(&result)) {
            throw std::runtime_error("mailbox creation failed: " +
                                     std::to_string(static_cast<int>(*error)));
        }
        return std::shared_ptr<NativeMailbox>(
            std::move(std::get<std::unique_ptr<NativeMailbox>>(result)));
    }
};
} // namespace

TEST_F(NativeShutdownTest, CancelsOperationAndDrainsRealDiscoveryBeforeFinalCallback) {
    ShutdownCoordinator lifecycle(loop.executor());
    auto gate = lifecycle.gate();
    std::vector<int> events;
    MailboxCallbacks handlers{
        [&](Bytes) { EXPECT_TRUE(gate.post([&] { events.push_back(1); })); },
        [&](MailboxError, std::optional<StoreError>) { ADD_FAILURE() << "mailbox error"; },
        [&](bool) {}};
    auto mailbox = make_mailbox(handlers);
    ASSERT_TRUE(lifecycle.track([mailbox] { mailbox->close(); }));
    auto publisher = std::make_shared<NativePublisher>(NativeStore(writer, limits), address,
                                                       MemoryBudget(512), loop.executor());
    ASSERT_TRUE(lifecycle.track([publisher] { publisher->close(); }));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    bool published = false;
    ASSERT_TRUE(
        std::holds_alternative<std::monostate>(publisher->send(Bytes{std::byte{1}}, [&](bool ok) {
            EXPECT_TRUE(ok);
            published = true;
        })));
    ASSERT_TRUE(loop.until([&] { return !events.empty() && published; }));
    CompletionSource<Unit> operation(loop.executor(), [&](auto result) {
        EXPECT_EQ(std::get<OperationError>(result), OperationError::Canceled);
        EXPECT_TRUE(gate.post([&] { events.push_back(2); }));
    });
    ASSERT_TRUE(lifecycle.track([handle = operation.operation()] { (void)handle.cancel(); }));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->send(
        Bytes{std::byte{2}}, [&](bool) { ADD_FAILURE() << "late publication callback"; })));
    unsigned submitted_at_final = 0;
    ASSERT_TRUE(lifecycle.begin([&] {
        submitted_at_final = loop.submitted();
        events.push_back(3);
    }));
    ASSERT_TRUE(loop.until([&] { return !events.empty() && events.back() == 3; }));
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
    writer->join();
    reader->join();
    EXPECT_EQ(loop.submitted(), submitted_at_final);
    loop.drain();
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
}
