#pragma once

#include "../src/discovery/native_store.hpp"
#include "../src/discovery/discovery_window.hpp"
#include "../src/discovery/native_advertisement_lease.hpp"
#include "../src/discovery/native_discovery_watch.hpp"
#include "../src/discovery/native_publisher.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <future>
#include <limits>
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
            ++submitted_;
        };
    }
    bool has_tasks() {
        std::lock_guard lock(mutex_);
        return !tasks_.empty();
    }
    std::size_t submitted() {
        std::lock_guard lock(mutex_);
        return submitted_;
    }
    bool until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done())
                return true;
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
    std::size_t submitted_ = 0;
};

class NativeDiscoveryWatchTest : public ::testing::Test {
  protected:
    Loop loop;
    std::shared_ptr<dht::DhtRunner> first = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> second = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<Bytes> capability = std::make_shared<Bytes>(32, std::byte{8});
    std::shared_ptr<Identity> identity = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    std::uint64_t time = 100;
    std::vector<AdvertisementEvent> events;
    std::vector<Advertisement> expired;
    std::vector<DiscoveryWatchError> errors;
    std::optional<bool> caught_up;
    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345684;
        config.peer_discovery = false;
        config.peer_publish = false;
        first->run("127.0.0.1", "::1", "0", config);
        second->run("127.0.0.1", "::1", "0", config);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        second->bootstrap(first->getBound(), [done](bool success) { done->set_value(success); });
        ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(future.get());
    }
    void TearDown() override {
        first->join();
        second->join();
    }
    DiscoveryWatchCallbacks callbacks() {
        return {[this](Advertisement, AdvertisementEvent event) { events.push_back(event); },
                [this](Advertisement value) { expired.push_back(value); },
                [this](DiscoveryWatchError error, std::optional<StoreError>) {
                    errors.push_back(error);
                },
                [this](bool value) { caught_up = value; }};
    }
    bool publish_at(ByteView key, ByteView wire) {
        NativeStore store(first, {1024, 32, 32768});
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        if (store.put(key, wire, [done](bool value) { done->set_value(value); }))
            return false;
        return future.wait_for(10s) == std::future_status::ready && future.get();
    }
    bool publish(ByteView wire) {
        return publish_at(std::get<DiscoveryKey>(derive_discovery_key(*capability)), wire);
    }
    bool publish_windowed(ByteView wire, std::uint64_t now) {
        return publish_at(std::get<DiscoveryKey>(
                              derive_discovery_window_key(*capability, discovery_window_for(now))),
                          wire);
    }
    Bytes advertisement(std::uint64_t sequence, std::uint64_t issued, std::uint64_t expiry) {
        AdvertisementFields fields{RecipientPublicKey{std::byte{2}},
                                   InstanceId{std::byte{3}},
                                   SignalingInboxKey{std::byte{4}},
                                   sequence,
                                   0,
                                   issued,
                                   expiry};
        return std::get<Bytes>(encode_advertisement(*identity, fields, *capability, {time, 60, 5}));
    }
};

[[maybe_unused]] AdvertisementLeaseConfig lease_config(std::uint64_t sequence = 0,
                                                       std::size_t max_publications = 4) {
    return {{RecipientPublicKey{std::byte{2}}, InstanceId{std::byte{3}},
             SignalingInboxKey{std::byte{4}}, sequence, 0, 0, 0},
            20,
            10,
            60,
            5,
            max_publications,
            {2048, 1, 1024}};
}
} // namespace
