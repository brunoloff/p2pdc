#include "../src/discovery/native_mailbox.hpp"
#include "../src/discovery/native_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
constexpr std::array<std::byte, 20> address{std::byte{22}};
class MailboxCloseTest : public ::testing::Test {
  protected:
    std::shared_ptr<dht::DhtRunner> writer = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> reader = std::make_shared<dht::DhtRunner>();
    std::mutex mutex;
    std::vector<std::function<void()>> tasks;
    std::atomic<unsigned> submitted{0};
    unsigned callbacks = 0;

    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345687;
        config.peer_discovery = false;
        config.peer_publish = false;
        writer->run("127.0.0.1", "::1", "0", config);
        reader->run("127.0.0.1", "::1", "0", config);
        auto connected = std::make_shared<std::promise<bool>>();
        auto ready = connected->get_future();
        reader->bootstrap(writer->getBound(), [connected](bool ok) { connected->set_value(ok); });
        ASSERT_EQ(ready.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(ready.get());
    }
    void TearDown() override {
        writer->join();
        reader->join();
    }
    Executor executor() {
        return [this](auto task) {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
            ++submitted;
        };
    }
    MailboxCallbacks handlers() {
        return {[this](Bytes) { ++callbacks; },
                [this](MailboxError, std::optional<StoreError>) { ++callbacks; },
                [this](bool) { ++callbacks; }};
    }
};
} // namespace

TEST_F(MailboxCloseTest, PendingCatchupAndLaterWritesDoNotSubmitAfterClose) {
    MemoryBudget budget(1024);
    NativeStore store(reader, {256, 4, 1024});
    auto result =
        NativeMailbox::create(store, address, {1024, 4, 256}, budget, executor(), handlers());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeMailbox>>(result));
    auto mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(result));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(mailbox->catch_up()));
    mailbox->close();
    const auto at_close = submitted.load();
    auto done = std::make_shared<std::promise<bool>>();
    auto published = done->get_future();
    NativeStore sender(writer, {256, 4, 1024});
    ASSERT_FALSE(
        sender.put(address, Bytes{std::byte{1}}, [done](bool ok) { done->set_value(ok); }));
    ASSERT_EQ(published.wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(published.get());
    reader->join();
    EXPECT_EQ(submitted.load(), at_close);
    for (auto &task : tasks) {
        task();
    }
    EXPECT_EQ(callbacks, 0U);
    EXPECT_EQ(budget.used(), 0U);
}

TEST_F(MailboxCloseTest, CloseSuppressesAnAlreadyQueuedWatchDelivery) {
    NativeStore store(reader, {256, 4, 1024});
    MemoryBudget budget(1024);
    auto result =
        NativeMailbox::create(store, address, {1024, 4, 256}, budget, executor(), handlers());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeMailbox>>(result));
    auto mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(result));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    auto done = std::make_shared<std::promise<bool>>();
    auto published = done->get_future();
    NativeStore sender(writer, {256, 4, 1024});
    ASSERT_FALSE(
        sender.put(address, Bytes{std::byte{1}}, [done](bool ok) { done->set_value(ok); }));
    ASSERT_EQ(published.wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(published.get());
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (submitted.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_GT(submitted.load(), 0U);
    EXPECT_GT(budget.used(), 0U);
    mailbox->close();
    reader->join();
    for (auto &task : tasks) {
        task();
    }
    EXPECT_EQ(callbacks, 0U);
    EXPECT_EQ(budget.used(), 0U);
}
