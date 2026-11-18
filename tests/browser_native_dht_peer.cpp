#include "p2pdc/node.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace p2pdc;
using namespace std::chrono_literals;

namespace {
class Loop {
    struct Timer {
        std::chrono::steady_clock::time_point due;
        std::function<void()> task;
        std::atomic<bool> canceled{false};
    };
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::shared_ptr<Timer>> timers_;

  public:
    Executor executor() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    Scheduler scheduler() {
        return [this](auto delay, std::function<void()> task) -> CancelTimer {
            auto timer = std::make_shared<Timer>();
            timer->due = std::chrono::steady_clock::now() + delay;
            timer->task = std::move(task);
            {
                std::lock_guard lock(mutex_);
                timers_.push_back(timer);
            }
            return [timer] { timer->canceled = true; };
        };
    }
    void pump() {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard lock(mutex_);
            work.swap(tasks_);
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(timers_, [&](const auto &timer) {
                if (timer->canceled)
                    return true;
                if (timer->due <= now) {
                    work.push_back(std::move(timer->task));
                    return true;
                }
                return false;
            });
        }
        for (auto &task : work)
            task();
    }
    bool until(const std::function<bool()> &condition, std::chrono::seconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (condition())
                return true;
            std::this_thread::sleep_for(2ms);
        }
        pump();
        return condition();
    }
};
class Peer {
    Loop loop_;
    std::unique_ptr<Node> node_;
    std::unique_ptr<Lease> lease_;
    std::unique_ptr<Subscription> watch_;
    std::unique_ptr<Connection> connection_;
    std::unique_ptr<IncomingListener> listener_;
    std::optional<Advertisement> peer_;
    std::optional<Advertisement> request_;
    SigningPublicKey expected_{};
    bool started_ = false, published_ = false, sent_ = false, received_ = false,
         acknowledged_ = false, datagram_received_ = false;
    bool require_datagram_ = std::getenv("P2PDC_TEST_REQUIRE_DATAGRAM") != nullptr;
    std::chrono::steady_clock::time_point next_datagram_{};
    std::string error_;
    std::string stop_input_;
    bool answerer_ = false;
    bool windowed_ = true;
    bool large_ = false;
    std::uint32_t max_message_bytes_ = 256 * 1024;
    std::uint32_t expected_message_bytes_ = 256 * 1024;

  public:
    Peer() {
        if (const char *large = std::getenv("P2PDC_TEST_LARGE_MESSAGE")) {
            if (std::string(large) != "1")
                throw std::invalid_argument("invalid large message setting");
            large_ = true;
            max_message_bytes_ = expected_message_bytes_ = 4 * 1024 * 1024;
        }
        if (const char *windowed = std::getenv("P2PDC_TEST_WINDOWED")) {
            if (std::string(windowed) != "1" && std::string(windowed) != "0")
                throw std::invalid_argument("invalid test discovery protocol");
            windowed_ = std::string(windowed) == "1";
        }
        if (const char *role = std::getenv("P2PDC_TEST_NATIVE_ROLE")) {
            if (std::string(role) != "answerer")
                throw std::invalid_argument("invalid test role");
            answerer_ = true;
        }
        if (const char *small = std::getenv("P2PDC_TEST_SMALL_MESSAGE_LIMIT")) {
            if (std::string(small) != "1")
                throw std::invalid_argument("invalid test message limit");
            max_message_bytes_ = 8192;
            expected_message_bytes_ = 8192;
        }
        if (const char *small = std::getenv("P2PDC_TEST_BROWSER_SMALL")) {
            if (std::string(small) != "1")
                throw std::invalid_argument("invalid browser test message limit");
            expected_message_bytes_ = 8192;
        }
        auto identity = Identity::import_seed(Bytes(32, std::byte{2}));
        if (!std::holds_alternative<Identity>(identity))
            throw std::runtime_error("test identity initialization failed");
        expected_ = std::get<Identity>(identity).public_key();
    }
    bool start();
    bool run();
    bool shutdown();
    const std::string &error() const { return error_; }

  private:
    bool discover();
    bool connect();
    bool listen();
    bool accept();
    ConnectionHandlers handlers();
    void send_datagram();
    void message(Bytes bytes);
    Bytes large_message() const;
    bool stop_requested();
    ConnectOptions connection_options() const;
};
ConnectOptions Peer::connection_options() const {
    ConnectOptions options;
    options.max_message_bytes = max_message_bytes_;
    options.max_pending_bytes =
        std::max<std::size_t>(options.max_pending_bytes, max_message_bytes_);
    return options;
}
bool Peer::start() {
    NativeDiscoveryConfig config;
    config.network_id = 873198;
    config.bind_addresses = NativeBindAddresses{"127.0.0.1", "::1"};
    config.bootstrap_servers = {{"127.0.0.1", 49411}};
    auto created = Node::create_with_identity(config, loop_.executor(), Bytes(32, std::byte{1}),
                                              Bytes(32, std::byte{3}));
    if (!std::holds_alternative<std::unique_ptr<Node>>(created)) {
        error_ = "test identity import failed";
        return false;
    }
    node_ = std::move(std::get<std::unique_ptr<Node>>(created));
    if (!node_ || !node_->start([this](NodeStartResult result) {
            if (result.status == NodeStartStatus::Started)
                started_ = true;
            else
                error_ = "node startup failed";
        }))
        return false;
    return loop_.until([this] { return started_ || !error_.empty(); }, 10s) && started_;
}
bool Peer::discover() {
    DiscoveryCapability capability{};
    capability.fill(std::byte{5});
    WatchOptions watch_options;
    AnnounceOptions announce_options;
    if (windowed_) {
        watch_options.protocol = DiscoveryProtocol::RotatingKeysV2;
        announce_options.protocol = DiscoveryProtocol::RotatingKeysV2;
    } else {
        watch_options.protocol = DiscoveryProtocol::FixedKeyV1;
        announce_options.protocol = DiscoveryProtocol::FixedKeyV1;
    }
    auto result =
        node_->watch(capability, loop_.scheduler(), watch_options,
                     {[this](Advertisement peer, PeerEvent event) {
                          if (event != PeerEvent::Expired && peer.signing_public_key == expected_)
                              peer_ = std::move(peer);
                      },
                      [](bool) {}, [this](WatchError) { error_ = "watch failed"; }});
    if (!std::holds_alternative<std::unique_ptr<Subscription>>(result))
        return false;
    watch_ = std::move(std::get<std::unique_ptr<Subscription>>(result));
    auto announced = node_->announce(capability, loop_.scheduler(), announce_options,
                                     {[this] { published_ = true; },
                                      [this](AnnouncementError e) {
                                          if (e != AnnouncementError::Retrying)
                                              error_ = "announcement failed";
                                      }});
    if (!std::holds_alternative<std::unique_ptr<Lease>>(announced))
        return false;
    lease_ = std::move(std::get<std::unique_ptr<Lease>>(announced));
    return true;
}
bool Peer::connect() {
    if (!peer_ || !watch_->is_current(*peer_))
        return false;
    auto result = node_->connect(*lease_, *watch_, *peer_, {}, loop_.scheduler(),
                                 connection_options(), handlers());
    if (!std::holds_alternative<std::unique_ptr<Connection>>(result)) {
        error_ = "connection admission failed";
        return false;
    }
    connection_ = std::move(std::get<std::unique_ptr<Connection>>(result));
    return true;
}
ConnectionHandlers Peer::handlers() {
    return {[](ConnectionState) {},
            [](RouteInfo) {},
            [this](Bytes value) { message(std::move(value)); },
            [] {},
            [this](ConnectError) { error_ = "connection failed"; },
            {},
            [this](Bytes value) {
                if (value == Bytes{std::byte{0x44}})
                    datagram_received_ = true;
            }};
}
// Datagrams may be lost, so they are repeated for as long as the test runs.
void Peer::send_datagram() {
    const auto now = std::chrono::steady_clock::now();
    if (!connection_ || !connection_->datagrams_available() || now < next_datagram_)
        return;
    next_datagram_ = now + 50ms;
    (void)connection_->send_datagram(Bytes{std::byte{0x45}});
}
bool Peer::listen() {
    auto result = node_->listen(*lease_, *watch_, loop_.scheduler(), {},
                                {[this](Advertisement peer) { request_ = std::move(peer); },
                                 [this](IncomingError) { error_ = "incoming listener failed"; }});
    if (!std::holds_alternative<std::unique_ptr<IncomingListener>>(result)) {
        error_ = "listener admission failed";
        return false;
    }
    listener_ = std::move(std::get<std::unique_ptr<IncomingListener>>(result));
    return true;
}
bool Peer::accept() {
    if (!request_ || request_->signing_public_key != expected_ || !watch_->is_current(*request_)) {
        error_ = "unexpected incoming request";
        return false;
    }
    auto result =
        node_->accept(*listener_, {}, loop_.scheduler(), connection_options(), handlers());
    if (!std::holds_alternative<std::unique_ptr<Connection>>(result)) {
        error_ = "incoming acceptance failed";
        return false;
    }
    connection_ = std::move(std::get<std::unique_ptr<Connection>>(result));
    return true;
}
void Peer::message(Bytes bytes) {
    if (!received_ &&
        bytes == (large_ ? large_message() : Bytes{std::byte{0}, std::byte{1}, std::byte{255}}))
        received_ = true;
    else if (received_ && !acknowledged_ && bytes == Bytes{std::byte{7}})
        acknowledged_ = true;
    else if (acknowledged_ && bytes == Bytes{std::byte{8}}) {
        if (connection_->send(Bytes{std::byte{9}}) != SendStatus::Accepted)
            error_ = "outage response failed";
    } else
        error_ = "browser message mismatch";
}
Bytes Peer::large_message() const {
    Bytes result(max_message_bytes_);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::byte>(i % 251);
    return result;
}
bool Peer::run() {
    if (!discover()) {
        error_ = "discovery admission failed";
        return false;
    }
    const bool complete = loop_.until(
        [this] {
            if (!error_.empty())
                return true;
            if (answerer_ && published_ && !listener_)
                listen();
            if (answerer_ && request_ && !connection_)
                accept();
            if (!answerer_ && published_ && peer_ && !connection_)
                connect();
            if (connection_ && connection_->state() == ConnectionState::Ready && !sent_) {
                if (connection_->max_message_size() != expected_message_bytes_)
                    error_ = "native negotiated limit mismatch";
                else if (connection_->send(large_ ? large_message()
                                                  : Bytes{std::byte{42}, std::byte{0}}) !=
                         SendStatus::Accepted)
                    error_ = "native send failed";
                else
                    sent_ = true;
            }
            send_datagram();
            return (sent_ && received_ && acknowledged_ &&
                    (datagram_received_ || !require_datagram_)) ||
                   !error_.empty();
        },
        45s);
    if (!complete && error_.empty())
        error_ = "exchange timed out";
    if (!error_.empty())
        return false;
    std::cout << "PASS" << std::endl;
    if (!loop_.until(
            [this] {
                send_datagram();
                return stop_requested() || !error_.empty();
            },
            45s))
        error_ = "test stop timed out";
    return error_.empty();
}
bool Peer::stop_requested() {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    if (poll(&descriptor, 1, 0) <= 0 || !(descriptor.revents & POLLIN))
        return false;
    char input[8]{};
    const auto size = read(STDIN_FILENO, input, sizeof(input));
    if (size <= 0) {
        error_ = "test stop input closed";
        return false;
    }
    stop_input_.append(input, static_cast<std::size_t>(size));
    if (stop_input_ == "STOP\n")
        return true;
    if (stop_input_.size() >= 5 ||
        std::string("STOP\n").compare(0, stop_input_.size(), stop_input_) != 0)
        error_ = "invalid test stop command";
    return false;
}
bool Peer::shutdown() {
    if (connection_)
        connection_->close();
    if (listener_)
        listener_->close();
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    if (!node_ || !node_->shutdown([stopped](NodeStopResult result) {
            *stopped = result.status == NodeStopStatus::Stopped;
        }))
        return false;
    return loop_.until([stopped] { return stopped->load(); }, 10s);
}
} // namespace

int main() {
    try {
        Peer peer;
        if (!peer.start() || !peer.run()) {
            std::cerr << "FAIL: " << peer.error() << std::endl;
            peer.shutdown();
            return 1;
        }
        if (!peer.shutdown()) {
            std::cerr << "FAIL: shutdown" << std::endl;
            return 1;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
