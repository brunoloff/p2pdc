#include "../src/discovery/native_store.hpp"
#include "../src/discovery/native_advertisement_lease.hpp"
#include "../src/discovery/native_discovery_watch.hpp"
#include "p2pdc/recipient_crypto.h"

#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class TestLoop {
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;

  public:
    Executor executor() {
        return [this](auto task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    void pump() {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard lock(mutex_);
            tasks.swap(tasks_);
        }
        for (auto &task : tasks)
            task();
    }
};

class WindowedPeer {
    TestLoop loop_;
    std::shared_ptr<dht::DhtRunner> runner_ = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<Bytes> capability_ = std::make_shared<Bytes>(32, std::byte{5});
    std::shared_ptr<Identity> identity_ = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    SigningPublicKey expected_ =
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{2}))).public_key();
    std::unique_ptr<NativeDiscoveryWatch> watch_;
    std::unique_ptr<NativeAdvertisementLease> lease_;
    std::uint64_t time_ = 1000;
    unsigned published_ = 0;
    std::optional<std::uint64_t> observed_;
    bool failed_ = false;

  public:
    bool start();
    bool run();
    void close();

  private:
    bool attach();
    bool step();
    bool until(const std::function<bool()> &done, std::chrono::seconds timeout);
};

bool WindowedPeer::start() {
    dht::DhtRunner::Config config;
    config.dht_config.node_config.network = 873198;
    config.peer_discovery = false;
    config.peer_publish = false;
    runner_->run("127.0.0.1", "::1", "0", config);
    auto ready = std::make_shared<std::promise<bool>>();
    auto future = ready->get_future();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(49411);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
        return false;
    runner_->bootstrap(dht::SockAddr(reinterpret_cast<sockaddr *>(&address), sizeof(address)),
                       [ready](bool ok) { ready->set_value(ok); });
    return future.wait_for(10s) == std::future_status::ready && future.get() && attach();
}

bool WindowedPeer::attach() {
    NativeStore store(runner_, {1024, 64, 65536});
    auto watched = NativeDiscoveryWatch::create(
        store, capability_, {{65536, 64, 1024}, 128, 300, 30, true}, MemoryBudget(131072),
        loop_.executor(), [this] { return time_; },
        {[this](Advertisement peer, AdvertisementEvent event) {
             if (event != AdvertisementEvent::Duplicate && peer.signing_public_key == expected_)
                 observed_ = peer.fields.sequence;
         },
         [](Advertisement) {}, [this](auto, auto) { failed_ = true; },
         [this](bool ok) {
             if (!ok)
                 failed_ = true;
         }});
    if (!std::holds_alternative<std::unique_ptr<NativeDiscoveryWatch>>(watched))
        return false;
    watch_ = std::move(std::get<std::unique_ptr<NativeDiscoveryWatch>>(watched));
    auto box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{3})));
    AdvertisementLeaseConfig config{
        {box.public_key(), InstanceId{std::byte{4}}, SignalingInboxKey{std::byte{5}}, 0, 0, 0, 0},
        120,
        60,
        300,
        30,
        8,
        {2048, 1, 1024},
        true};
    auto lease = NativeAdvertisementLease::create(
        store, identity_, capability_, config, MemoryBudget(4096), loop_.executor(),
        [this] { return time_; },
        {[this](auto) { ++published_; }, [this](auto) { failed_ = true; }});
    if (!std::holds_alternative<std::unique_ptr<NativeAdvertisementLease>>(lease))
        return false;
    lease_ = std::move(std::get<std::unique_ptr<NativeAdvertisementLease>>(lease));
    return true;
}

bool WindowedPeer::step() {
    loop_.pump();
    if (!watch_->tick())
        return false;
    auto result = lease_->tick();
    return !failed_ && result != AdvertisementLeaseTick::Failed &&
           result != AdvertisementLeaseTick::Closed;
}
bool WindowedPeer::until(const std::function<bool()> &done, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!step())
            return false;
        if (done())
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}
bool WindowedPeer::run() {
    if (!until([this] { return published_ >= 1 && observed_ == 0; }, 20s))
        return false;
    std::cout << "W 01" << std::endl;
    if (!until(
            [] {
                pollfd input{STDIN_FILENO, POLLIN, 0};
                return poll(&input, 1, 0) > 0;
            },
            20s))
        return false;
    std::string command;
    if (!std::getline(std::cin, command) || command != "DONE")
        return false;
    time_ = 1120;
    if (!until([this] { return published_ >= 2 && observed_ == 1; }, 20s))
        return false;
    std::cout << "PASS" << std::endl;
    if (!until(
            [] {
                pollfd input{STDIN_FILENO, POLLIN, 0};
                return poll(&input, 1, 0) > 0;
            },
            20s))
        return false;
    return std::getline(std::cin, command) && command == "STOP";
}
void WindowedPeer::close() {
    if (watch_)
        watch_->close();
    if (lease_)
        lease_->close();
    runner_->join();
    loop_.pump();
}
} // namespace

int main() {
    WindowedPeer peer;
    const bool ok = peer.start() && peer.run();
    peer.close();
    if (!ok)
        std::cerr << "windowed native discovery failed\n";
    return ok ? 0 : 1;
}
