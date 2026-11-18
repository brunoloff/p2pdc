#include "p2pdc/node.h"
#include "p2pdc/runtime.h"

#include <emscripten.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace p2pdc;
using namespace std::chrono_literals;

EM_JS(void, report_recovery_stage, (int stage), {
    document.body.dataset.stage = stage == 1   ? "await-outage"
                                  : stage == 2 ? "reconnecting"
                                               : "recovered";
});

namespace {
class BrowserNativePeer : public std::enable_shared_from_this<BrowserNativePeer> {
  public:
    BrowserNativePeer(Runtime &runtime, bool offerer, bool recovery,
                      std::function<void(bool)> completion);
    void start();

  private:
    void started(NodeStartResult result);
    bool watch();
    bool announce();
    void check_discovery();
    bool listen();
    bool connect();
    void request(Advertisement peer);
    ConnectionHandlers handlers();
    void state(ConnectionState state);
    void message(Bytes value);
    void check_exchange();
    void send_datagram();
    void discovery_status(DiscoveryStatus status);
    void finish(bool success);
    void stopped(NodeStopResult result);
    Runtime &runtime_;
    bool offerer_;
    bool recovery_;
    std::function<void(bool)> completion_;
    std::shared_ptr<BrowserNativePeer> keepalive_;
    std::unique_ptr<Node> node_;
    std::unique_ptr<Lease> lease_;
    std::unique_ptr<Subscription> watch_;
    std::unique_ptr<IncomingListener> listener_;
    std::unique_ptr<Connection> connection_;
    std::optional<Advertisement> peer_;
    SigningPublicKey native_key_{};
    DiscoveryCapability capability_{};
    CancelTimer deadline_;
    CancelTimer settle_;
    CancelTimer datagram_timer_;
    bool datagram_received_ = false;
    bool published_ = false, caught_ = false, connecting_ = false, ready_ = false;
    bool sent_ = false, received_ = false, acknowledged_ = false, finishing_ = false;
    bool success_ = false, awaiting_outage_ = false, saw_reconnecting_ = false, recovered_ = false;
};

BrowserNativePeer::BrowserNativePeer(Runtime &runtime, bool offerer, bool recovery,
                                     std::function<void(bool)> completion)
    : runtime_(runtime), offerer_(offerer), recovery_(recovery),
      completion_(std::move(completion)) {
    capability_.fill(std::byte{5});
    const auto native = Identity::import_seed(Bytes(32, std::byte{1}));
    if (!std::holds_alternative<Identity>(native))
        throw std::runtime_error("native test identity failed");
    native_key_ = std::get<Identity>(native).public_key();
    DiscoveryConfig config;
    config.browser_proxy_urls = {"http://127.0.0.1:49412"};
    auto result = Node::create_with_identity(config, runtime_.executor(), Bytes(32, std::byte{2}),
                                             Bytes(32, std::byte{4}));
    auto *node = std::get_if<std::unique_ptr<Node>>(&result);
    if (!node)
        throw std::runtime_error("browser test identity failed");
    node_ = std::move(*node);
}
void BrowserNativePeer::start() {
    keepalive_ = shared_from_this();
    auto weak = weak_from_this();
    deadline_ = runtime_.scheduler()(35s, [weak] {
        if (auto self = weak.lock()) {
            std::cerr << "native/browser public exchange timed out\n";
            self->finish(false);
        }
    });
    if (!node_->start([weak](NodeStartResult result) {
            if (auto self = weak.lock())
                self->started(std::move(result));
        }))
        finish(false);
}
void BrowserNativePeer::started(NodeStartResult result) {
    if (result.status != NodeStartStatus::Started || !watch() || !announce())
        finish(false);
}
bool BrowserNativePeer::watch() {
    auto weak = weak_from_this();
    DiscoveryHandlers handlers{
        [weak](Advertisement peer, PeerEvent event) {
            if (auto self = weak.lock(); self && event != PeerEvent::Expired &&
                                         peer.signing_public_key == self->native_key_) {
                self->peer_ = std::move(peer);
                self->check_discovery();
            }
        },
        [weak](bool ok) {
            if (auto self = weak.lock()) {
                self->caught_ = ok;
                if (ok)
                    self->check_discovery();
                else
                    self->finish(false);
            }
        },
        [weak](WatchError error) {
            if (auto self = weak.lock()) {
                std::cerr << "browser watch failed " << static_cast<int>(error) << '\n';
                self->finish(false);
            }
        },
        [weak](DiscoveryStatus status) {
            if (auto self = weak.lock())
                self->discovery_status(status);
        }};
    WatchOptions options;
    options.pulse_interval = 100ms;
    auto result = node_->watch(capability_, runtime_.scheduler(), options, std::move(handlers));
    auto *watch = std::get_if<std::unique_ptr<Subscription>>(&result);
    if (!watch)
        return false;
    watch_ = std::move(*watch);
    return true;
}
bool BrowserNativePeer::announce() {
    auto weak = weak_from_this();
    AnnounceHandlers handlers{[weak] {
                                  if (auto self = weak.lock()) {
                                      self->published_ = true;
                                      self->check_discovery();
                                  }
                              },
                              [weak](AnnouncementError error) {
                                  if (error != AnnouncementError::Retrying)
                                      if (auto self = weak.lock())
                                          self->finish(false);
                              }};
    AnnounceOptions options;
    options.pulse_interval = 100ms;
    auto result = node_->announce(capability_, runtime_.scheduler(), options, std::move(handlers));
    auto *lease = std::get_if<std::unique_ptr<Lease>>(&result);
    if (!lease)
        return false;
    lease_ = std::move(*lease);
    return true;
}
void BrowserNativePeer::check_discovery() {
    if (finishing_ || connecting_ || !published_ || !caught_ || !lease_ || !watch_)
        return;
    if (!offerer_) {
        connecting_ = true;
        if (!listen())
            finish(false);
    } else if (peer_) {
        connecting_ = true;
        if (!connect())
            finish(false);
    }
}
bool BrowserNativePeer::listen() {
    auto weak = weak_from_this();
    auto result = node_->listen(*lease_, *watch_, runtime_.scheduler(), {},
                                {[weak](Advertisement peer) {
                                     if (auto self = weak.lock())
                                         self->request(std::move(peer));
                                 },
                                 [weak](IncomingError error) {
                                     if (auto self = weak.lock()) {
                                         std::cerr << "browser incoming failed "
                                                   << static_cast<int>(error) << '\n';
                                         self->finish(false);
                                     }
                                 }});
    auto *listener = std::get_if<std::unique_ptr<IncomingListener>>(&result);
    if (!listener)
        return false;
    listener_ = std::move(*listener);
    return true;
}
bool BrowserNativePeer::connect() {
    auto result =
        node_->connect(*lease_, *watch_, *peer_, {}, runtime_.scheduler(), {}, handlers());
    auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!connection)
        return false;
    connection_ = std::move(*connection);
    return true;
}
void BrowserNativePeer::request(Advertisement peer) {
    if (peer.signing_public_key != native_key_ || !watch_->is_current(peer)) {
        finish(false);
        return;
    }
    auto result = node_->accept(*listener_, {}, runtime_.scheduler(), {}, handlers());
    auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!connection) {
        finish(false);
        return;
    }
    connection_ = std::move(*connection);
}
ConnectionHandlers BrowserNativePeer::handlers() {
    auto weak = weak_from_this();
    return {[weak](ConnectionState value) {
                if (auto self = weak.lock())
                    self->state(value);
            },
            [](RouteInfo) {},
            [weak](Bytes value) {
                if (auto self = weak.lock())
                    self->message(std::move(value));
            },
            [] {},
            [weak](ConnectError error) {
                if (auto self = weak.lock()) {
                    std::cerr << "browser connection closed " << static_cast<int>(error) << '\n';
                    self->finish(self->recovery_
                                     ? self->recovered_
                                     : self->sent_ && self->received_ && self->acknowledged_);
                }
            },
            {},
            [weak](Bytes value) {
                if (auto self = weak.lock(); self && value == Bytes{std::byte{0x45}}) {
                    self->datagram_received_ = true;
                    self->check_exchange();
                }
            }};
}
// Datagrams may be lost, so they are repeated until the test ends.
void BrowserNativePeer::send_datagram() {
    if (finishing_ || !connection_)
        return;
    if (connection_->datagrams_available())
        (void)connection_->send_datagram(Bytes{std::byte{0x44}});
    auto weak = weak_from_this();
    datagram_timer_ = runtime_.scheduler()(50ms, [weak] {
        if (auto self = weak.lock())
            self->send_datagram();
    });
}
void BrowserNativePeer::state(ConnectionState value) {
    if (value == ConnectionState::Failed) {
        finish(recovery_ ? recovered_ : sent_ && received_ && acknowledged_);
        return;
    }
    if (value != ConnectionState::Ready || sent_)
        return;
    ready_ = true;
    if (connection_->send(Bytes{std::byte{0}, std::byte{1}, std::byte{255}}) !=
        SendStatus::Accepted) {
        finish(false);
        return;
    }
    sent_ = true;
    send_datagram();
    check_exchange();
}
void BrowserNativePeer::message(Bytes value) {
    if (recovery_ && saw_reconnecting_ && value == Bytes{std::byte{9}}) {
        report_recovery_stage(2);
        return;
    }
    if (value != Bytes{std::byte{42}, std::byte{0}} || received_) {
        finish(false);
        return;
    }
    received_ = true;
    if (connection_->send(Bytes{std::byte{7}}) != SendStatus::Accepted) {
        finish(false);
        return;
    }
    acknowledged_ = true;
    check_exchange();
}
void BrowserNativePeer::check_exchange() {
    if (ready_ && sent_ && received_ && acknowledged_ && datagram_received_ && !settle_) {
        if (recovery_) {
            awaiting_outage_ = true;
            report_recovery_stage(1);
            return;
        }
        auto weak = weak_from_this();
        settle_ = runtime_.scheduler()(1s, [weak] {
            if (auto self = weak.lock())
                self->finish(true);
        });
    }
}
void BrowserNativePeer::discovery_status(DiscoveryStatus status) {
    if (!recovery_ || !awaiting_outage_ || finishing_)
        return;
    if (!connection_ || connection_->state() != ConnectionState::Ready) {
        finish(false);
        return;
    }
    if (status == DiscoveryStatus::Reconnecting) {
        if (!watch_->recovering() || watch_->healthy()) {
            finish(false);
            return;
        }
        saw_reconnecting_ = true;
        if (connection_->send(Bytes{std::byte{8}}) != SendStatus::Accepted)
            finish(false);
    } else if (saw_reconnecting_) {
        if (!watch_->healthy() || watch_->recovering()) {
            finish(false);
            return;
        }
        recovered_ = true;
        report_recovery_stage(3);
    }
}
void BrowserNativePeer::finish(bool success) {
    if (finishing_)
        return;
    finishing_ = true;
    success_ = success;
    if (deadline_)
        deadline_();
    if (settle_)
        settle_();
    if (datagram_timer_)
        datagram_timer_();
    auto weak = weak_from_this();
    if (!node_->shutdown([weak](NodeStopResult result) {
            if (auto self = weak.lock())
                self->stopped(std::move(result));
        }))
        stopped(NodeStopResult{NodeStopStatus::BackendFailure, {}});
}
void BrowserNativePeer::stopped(NodeStopResult result) {
    completion_(success_ && result.status == NodeStopStatus::Stopped);
    keepalive_.reset();
}
} // namespace

void run_browser_native_public_peer(Runtime &runtime, bool offerer, bool recovery,
                                    std::function<void(bool)> completion) {
    auto peer =
        std::make_shared<BrowserNativePeer>(runtime, offerer, recovery, std::move(completion));
    peer->start();
}
