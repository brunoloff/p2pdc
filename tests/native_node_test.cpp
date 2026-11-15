#include "../src/discovery/native_node_state.hpp"
#include "p2pdc/node.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

using namespace p2pdc;
using namespace std::chrono_literals;

namespace p2pdc::detail {
struct NodeTestAccess {
    static Executor backend(Node &node) { return node.state_->executor; }
};
} // namespace p2pdc::detail

namespace {
class Queue {
  public:
    Executor executor() {
        return [this](auto task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
            ready_.notify_one();
        };
    }
    bool run_one() {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, 10s, [&] { return !tasks_.empty(); })) {
            return false;
        }
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        task();
        return true;
    }

  private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
};

NativeDiscoveryConfig local_config() {
    NativeDiscoveryConfig config;
    config.network_id = 0x12345694;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    return config;
}
} // namespace

TEST(NativeNode, StartsAndShutsDownAsynchronously) {
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    std::vector<int> events;
    ASSERT_TRUE(node->start([&](auto result) {
        EXPECT_EQ(result.status, NodeStartStatus::Started);
        EXPECT_FALSE(result.error);
        events.push_back(1);
    }));
    EXPECT_FALSE(node->start([](auto) {}));
    EXPECT_TRUE(events.empty());
    ASSERT_TRUE(queue.run_one()); // backend startup
    ASSERT_TRUE(queue.run_one()); // application completion
    ASSERT_EQ(events, (std::vector<int>{1}));
    ASSERT_TRUE(node->shutdown([&](auto result) {
        EXPECT_EQ(result.status, NodeStopStatus::Stopped);
        EXPECT_FALSE(result.error);
        events.push_back(2);
    }));
    EXPECT_FALSE(node->shutdown([](auto) {}));
    ASSERT_TRUE(queue.run_one()); // joined runner
    ASSERT_TRUE(queue.run_one()); // final application callback
    EXPECT_EQ(events, (std::vector<int>{1, 2}));
    EXPECT_TRUE(node->healthy());
}

TEST(NativeNode, ShutdownCancelsStartupBeforeFinalCallback) {
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    std::vector<int> events;
    ASSERT_TRUE(node->start([&](auto result) {
        EXPECT_EQ(result.status, NodeStartStatus::Canceled);
        events.push_back(1);
    }));
    ASSERT_TRUE(node->shutdown([&](auto result) {
        EXPECT_EQ(result.status, NodeStopStatus::Stopped);
        events.push_back(2);
    }));
    while (events.size() < 2) {
        ASSERT_TRUE(queue.run_one());
    }
    EXPECT_EQ(events, (std::vector<int>{1, 2}));
}

TEST(NativeNode, ShutdownWaitsForNestedBackendWork) {
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    const auto backend = p2pdc::detail::NodeTestAccess::backend(*node);
    std::vector<int> events;
    backend([&] {
        events.push_back(1);
        backend([&] {
            events.push_back(2);
            backend([&] { events.push_back(3); });
        });
    });
    ASSERT_TRUE(node->shutdown([&](auto result) {
        EXPECT_EQ(result.status, NodeStopStatus::Stopped);
        events.push_back(4);
    }));
    while (events.size() < 4)
        ASSERT_TRUE(queue.run_one());
    EXPECT_EQ(events, (std::vector{1, 2, 3, 4}));
    EXPECT_THROW(backend([] {}), std::logic_error);
}

TEST(NativeNode, ShutdownBeforeStartAndInvalidArguments) {
    Queue queue;
    EXPECT_THROW(static_cast<void>(Node::create(local_config(), {})), std::invalid_argument);
    auto invalid = local_config();
    invalid.bootstrap_servers = {{"bad host", 4222}};
    EXPECT_THROW(static_cast<void>(Node::create(invalid, queue.executor())), std::invalid_argument);
    auto node = Node::create(local_config(), queue.executor());
    EXPECT_THROW(static_cast<void>(node->start({})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(node->shutdown({})), std::invalid_argument);
    bool stopped = false;
    ASSERT_TRUE(
        node->shutdown([&](auto result) { stopped = result.status == NodeStopStatus::Stopped; }));
    EXPECT_FALSE(node->start([](auto) {}));
    ASSERT_TRUE(queue.run_one());
    ASSERT_TRUE(queue.run_one());
    EXPECT_TRUE(stopped);
}

TEST(NativeNode, DestructionSuppressesQueuedStartupCallback) {
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    bool called = false;
    ASSERT_TRUE(node->start([&](auto) { called = true; }));
    ASSERT_TRUE(queue.run_one()); // backend startup queues application completion
    node.reset();
    ASSERT_TRUE(queue.run_one());
    EXPECT_FALSE(called);
}

TEST(NativeNode, FailedFinalCallbackCopyCanBeRetried) {
    struct Callback {
        std::shared_ptr<bool> fail;
        std::shared_ptr<bool> called;
        Callback(std::shared_ptr<bool> should_fail, std::shared_ptr<bool> was_called)
            : fail(std::move(should_fail)), called(std::move(was_called)) {}
        Callback(const Callback &other) : fail(other.fail), called(other.called) {
            if (*fail) {
                throw std::bad_alloc();
            }
        }
        void operator()(NodeStopResult result) const {
            EXPECT_EQ(result.status, NodeStopStatus::Stopped);
            *called = true;
        }
    };
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    auto fail = std::make_shared<bool>(false);
    auto called = std::make_shared<bool>(false);
    std::function<void(NodeStopResult)> callback = Callback(fail, called);
    ASSERT_TRUE(node->shutdown(std::move(callback)));
    *fail = true;
    EXPECT_THROW(static_cast<void>(queue.run_one()), std::bad_alloc);
    EXPECT_FALSE(node->healthy());
    EXPECT_FALSE(*called);
    *fail = false;
    EXPECT_TRUE(node->retry_callbacks());
    EXPECT_FALSE(node->retry_callbacks());
    ASSERT_TRUE(queue.run_one());
    EXPECT_TRUE(*called);
    EXPECT_TRUE(node->healthy());
}

TEST(NativeNode, StartupCallbackCanRequestShutdown) {
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    std::vector<int> events;
    ASSERT_TRUE(node->start([&](auto result) {
        EXPECT_EQ(result.status, NodeStartStatus::Started);
        events.push_back(1);
        EXPECT_TRUE(node->shutdown([&](auto stopped) {
            EXPECT_EQ(stopped.status, NodeStopStatus::Stopped);
            events.push_back(2);
        }));
    }));
    while (events.size() < 2) {
        ASSERT_TRUE(queue.run_one());
    }
    EXPECT_EQ(events, (std::vector<int>{1, 2}));
}

TEST(NativeNode, StartupCallbackCanDestroyNode) {
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    ASSERT_TRUE(node->start([&](auto result) {
        EXPECT_EQ(result.status, NodeStartStatus::Started);
        node.reset();
    }));
    ASSERT_TRUE(queue.run_one());
    ASSERT_TRUE(queue.run_one());
    EXPECT_FALSE(node);
}

TEST(NativeNode, FinalCallbackCopyCanReenterRetry) {
    struct Callback {
        Node *node;
        std::shared_ptr<bool> armed;
        std::shared_ptr<bool> called;
        Callback(Node *owner, std::shared_ptr<bool> flag, std::shared_ptr<bool> result)
            : node(owner), armed(std::move(flag)), called(std::move(result)) {}
        Callback(const Callback &other)
            : node(other.node), armed(other.armed), called(other.called) {
            if (std::exchange(*armed, false)) {
                EXPECT_FALSE(node->retry_callbacks());
            }
        }
        void operator()(NodeStopResult) const { *called = true; }
    };
    Queue queue;
    auto node = Node::create(local_config(), queue.executor());
    auto armed = std::make_shared<bool>(false);
    auto called = std::make_shared<bool>(false);
    std::function<void(NodeStopResult)> callback = Callback(node.get(), armed, called);
    ASSERT_TRUE(node->shutdown(std::move(callback)));
    *armed = true;
    ASSERT_TRUE(queue.run_one());
    ASSERT_TRUE(queue.run_one());
    EXPECT_TRUE(*called);
    EXPECT_FALSE(*armed);
}
