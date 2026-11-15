#include "node_connection_consumer.hpp"
#include "node_ice_validation_smoke.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, set_test_webrtc_available, (int enabled), {
    if (enabled != 1) {
        Module.p2pdcSavedPeerConnection = globalThis.RTCPeerConnection;
        if (enabled == 2)
            globalThis.RTCPeerConnection = function() { throw new Error("test backend failure"); };
        else
            globalThis.RTCPeerConnection = undefined;
    } else {
        globalThis.RTCPeerConnection = Module.p2pdcSavedPeerConnection;
        delete Module.p2pdcSavedPeerConnection;
    }
});
#endif

namespace p2pdc_test {
using namespace p2pdc;
using namespace std::chrono_literals;
namespace {
struct Side {
    std::unique_ptr<Node> node;
    std::unique_ptr<Lease> lease;
    std::unique_ptr<Subscription> watch;
    std::unique_ptr<Connection> connection;
    std::optional<Advertisement> peer;
    bool published = false, caught = false, ready = false, received = false, routed = false;
};
class Consumer : public std::enable_shared_from_this<Consumer> {
  public:
    Consumer(DiscoveryConfig config, Executor executor, Scheduler scheduler, IceConfig ice,
             bool expect_turn, std::function<void(bool)> completion, int expected_backend_error,
             int test_accept_unsupported);
    void start();
    void cancel();

  private:
    void started(unsigned index, NodeStartResult result);
    bool watch(unsigned index);
    bool announce(unsigned index);
    void check_discovery();
    bool listen();
    bool connect();
    bool accepts_after_unavailable(Side &side);
    void request(Advertisement peer);
    ConnectionHandlers handlers(unsigned index);
    void state(unsigned index, ConnectionState value);
    void route(unsigned index, RouteInfo value);
    void message(unsigned index, Bytes value);
    void check_exchange();
    void repeat_exchange();
    void timed_out();
    void finish(bool success);
    void stopped(NodeStopResult result);
    std::array<Side, 2> sides_;
    std::array<std::unique_ptr<Connection>, 2> prior_;
    std::unique_ptr<IncomingListener> listener_;
    Scheduler scheduler_;
    IceConfig ice_;
    std::function<void(bool)> completion_;
    std::shared_ptr<Consumer> keepalive_;
    CancelTimer deadline_;
    DiscoveryCapability capability_{};
    bool expect_turn_, connecting_ = false, sent_ = false, finishing_ = false, success_ = false;
    int expected_backend_error_;
    int test_accept_unsupported_;
    unsigned stopped_ = 0, generation_ = 0;
};
Consumer::Consumer(DiscoveryConfig config, Executor executor, Scheduler scheduler, IceConfig ice,
                   bool expect_turn, std::function<void(bool)> completion,
                   int expected_backend_error, int test_accept_unsupported)
    : scheduler_(std::move(scheduler)), ice_(std::move(ice)), completion_(std::move(completion)),
      expect_turn_(expect_turn), expected_backend_error_(expected_backend_error),
      test_accept_unsupported_(test_accept_unsupported) {
    for (auto &side : sides_)
        side.node = Node::create(config, executor);
    capability_.fill(std::byte{49});
}
void Consumer::start() {
    keepalive_ = shared_from_this();
    auto weak = weak_from_this();
    deadline_ = scheduler_(25s, [weak] {
        if (auto self = weak.lock())
            self->timed_out();
    });
    for (unsigned index = 0; index < 2; ++index)
        if (!sides_[index].node->start([weak, index](NodeStartResult result) {
                if (auto self = weak.lock())
                    self->started(index, std::move(result));
            }))
            finish(false);
}
void Consumer::started(unsigned index, NodeStartResult result) {
    if (result.status != NodeStartStatus::Started || !watch(index) || !announce(index))
        finish(false);
}
bool Consumer::watch(unsigned index) {
    auto weak = weak_from_this();
    DiscoveryHandlers callbacks{
        [weak, index](Advertisement peer, PeerEvent event) {
            if (auto self = weak.lock(); self && event != PeerEvent::Expired) {
                auto identity = self->sides_[index].node->identity();
                if (identity && peer.signing_public_key != identity->signing_key) {
                    self->sides_[index].peer = std::move(peer);
                    self->check_discovery();
                }
            }
        },
        [weak, index](bool ok) {
            if (auto self = weak.lock()) {
                self->sides_[index].caught = ok;
                if (ok)
                    self->check_discovery();
                else
                    self->finish(false);
            }
        },
        [weak](WatchError error) {
            if (auto self = weak.lock()) {
                std::cerr << "watch error " << static_cast<int>(error) << '\n';
                self->finish(false);
            }
        }};
    WatchOptions options;
    options.pulse_interval = 100ms;
    auto result = sides_[index].node->watch(capability_, scheduler_, options, std::move(callbacks));
    auto *watch = std::get_if<std::unique_ptr<Subscription>>(&result);
    if (!watch)
        return false;
    sides_[index].watch = std::move(*watch);
    return true;
}
bool Consumer::announce(unsigned index) {
    auto weak = weak_from_this();
    AnnounceHandlers callbacks{[weak, index] {
                                   if (auto self = weak.lock()) {
                                       self->sides_[index].published = true;
                                       self->check_discovery();
                                   }
                               },
                               [weak](AnnouncementError error) {
                                   if (error != AnnouncementError::Retrying)
                                       if (auto self = weak.lock()) {
                                           std::cerr << "announce error " << static_cast<int>(error)
                                                     << '\n';
                                           self->finish(false);
                                       }
                               }};
    AnnounceOptions options;
    options.pulse_interval = 100ms;
    auto result =
        sides_[index].node->announce(capability_, scheduler_, options, std::move(callbacks));
    auto *lease = std::get_if<std::unique_ptr<Lease>>(&result);
    if (!lease)
        return false;
    sides_[index].lease = std::move(*lease);
    return true;
}
void Consumer::check_discovery() {
    if (finishing_ || connecting_)
        return;
    for (const auto &side : sides_)
        if (!side.published || !side.caught || !side.peer || !side.lease || !side.watch)
            return;
    connecting_ = true;
    if (!listen() || !connect())
        finish(false);
}
bool Consumer::listen() {
    auto weak = weak_from_this();
    IncomingHandlers callbacks{[weak](Advertisement peer) {
                                   if (auto self = weak.lock())
                                       self->request(std::move(peer));
                               },
                               [weak](IncomingError error) {
                                   if (auto self = weak.lock()) {
                                       std::cerr << "incoming error " << static_cast<int>(error)
                                                 << '\n';
                                       self->finish(false);
                                   }
                               }};
    auto &side = sides_[1];
    auto result = side.node->listen(*side.lease, *side.watch, scheduler_, {}, std::move(callbacks));
    auto *listener = std::get_if<std::unique_ptr<IncomingListener>>(&result);
    if (!listener)
        return false;
    listener_ = std::move(*listener);
    return true;
}
bool Consumer::connect() {
    auto &side = sides_[0];
    auto invalid = *side.peer;
    invalid.fields.instance_id[0] ^= std::byte{1};
    auto bad =
        side.node->connect(*side.lease, *side.watch, invalid, {}, scheduler_, {}, handlers(0));
    if (!std::holds_alternative<ConnectError>(bad) ||
        std::get<ConnectError>(bad) != ConnectError::InvalidPeer)
        return false;
    auto foreign = side.node->accept(*listener_, {}, scheduler_, {}, handlers(0));
    if (!std::holds_alternative<ConnectError>(foreign) ||
        std::get<ConnectError>(foreign) != ConnectError::InvalidPeer)
        return false;
    auto result =
        side.node->connect(*side.lease, *side.watch, *side.peer, ice_, scheduler_, {}, handlers(0));
    if (expected_backend_error_) {
        const auto *error = std::get_if<ConnectError>(&result);
        const auto expected =
            expected_backend_error_ == 1 ? ConnectError::Unsupported : ConnectError::Backend;
        finish(error && *error == expected);
        return true;
    }
    auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!connection) {
        std::cerr << "connect error " << static_cast<int>(std::get<ConnectError>(result)) << '\n';
        return false;
    }
    side.connection = std::move(*connection);
    return true;
}
#include "node_connection_accept.inc"
ConnectionHandlers Consumer::handlers(unsigned index) {
    auto weak = weak_from_this();
    const auto generation = generation_;
    return {[weak, index, generation](ConnectionState value) {
                if (auto self = weak.lock(); self && self->generation_ == generation)
                    self->state(index, value);
            },
            [weak, index, generation](RouteInfo value) {
                if (auto self = weak.lock(); self && self->generation_ == generation)
                    self->route(index, std::move(value));
            },
            [weak, index, generation](Bytes value) {
                if (auto self = weak.lock(); self && self->generation_ == generation)
                    self->message(index, std::move(value));
            },
            [] {},
            [weak](ConnectError error) {
                if (auto self = weak.lock()) {
                    std::cerr << "connection error " << static_cast<int>(error) << '\n';
                    self->finish(false);
                }
            },
            {}};
}
void Consumer::state(unsigned index, ConnectionState value) {
    if (value == ConnectionState::Failed) {
        finish(false);
        return;
    }
    if (value == ConnectionState::Ready) {
        sides_[index].ready = true;
        check_exchange();
    }
}
void Consumer::route(unsigned index, RouteInfo value) {
    if (!value.selected_pair) {
        sides_[index].routed = false;
    } else if (!expect_turn_) {
        const bool peer_reflexive = value.selected_pair->local == CandidateKind::PeerReflexive ||
                                    value.selected_pair->remote == CandidateKind::PeerReflexive;
        sides_[index].routed = value.current == RouteKind::Direct ||
                               (value.current == RouteKind::Unknown && peer_reflexive &&
                                value.turn_history != TurnHistory::Used);
    } else {
        const bool local = value.selected_pair->local == CandidateKind::Relayed;
        const bool remote = value.selected_pair->remote == CandidateKind::Relayed;
        sides_[index].routed = value.current == RouteKind::TurnRelayed &&
                               value.turn_history == TurnHistory::Used && (local || remote) &&
                               (ice_.relay_policy != RelayPolicy::RequireLocalRelay || local);
    }
    check_exchange();
}
void Consumer::message(unsigned index, Bytes value) {
    const Bytes expected =
        index == 0 ? Bytes{std::byte{0}, std::byte{255}} : Bytes(16384, std::byte{42});
    if (value != expected) {
        finish(false);
        return;
    }
    sides_[index].received = true;
    check_exchange();
}
#include "node_connection_exchange.inc"
void Consumer::timed_out() {
    for (const auto &side : sides_) {
        std::cerr << "peer published=" << side.published << " caught=" << side.caught
                  << " discovered=" << bool(side.peer) << " ready=" << side.ready
                  << " received=" << side.received << " routed=" << side.routed;
        if (side.connection) {
            const auto current = side.connection->route();
            std::cerr << " route=" << static_cast<int>(current.current)
                      << " history=" << static_cast<int>(current.turn_history);
            if (current.selected_pair)
                std::cerr << " local=" << static_cast<int>(current.selected_pair->local)
                          << " remote=" << static_cast<int>(current.selected_pair->remote);
        }
        std::cerr << '\n';
    }
    finish(false);
}
void Consumer::finish(bool success) {
    if (finishing_)
        return;
    finishing_ = true;
    success_ = success;
    if (deadline_)
        deadline_();
    auto weak = weak_from_this();
    for (auto &side : sides_)
        if (!side.node->shutdown([weak](NodeStopResult result) {
                if (auto self = weak.lock())
                    self->stopped(std::move(result));
            }))
            stopped(NodeStopResult{NodeStopStatus::BackendFailure, {}});
}
void Consumer::stopped(NodeStopResult result) {
    success_ = success_ && result.status == NodeStopStatus::Stopped;
    if (++stopped_ == 2) {
        completion_(success_);
        keepalive_.reset();
    }
}
void Consumer::cancel() {
    if (finishing_)
        return;
    finishing_ = true;
    if (deadline_)
        deadline_();
    listener_.reset();
    for (auto &side : sides_)
        side.node.reset();
    keepalive_.reset();
}
} // namespace

std::function<void()>
run_node_connection_consumer(DiscoveryConfig config, Executor executor, Scheduler scheduler,
                             IceConfig ice, bool expect_turn, std::function<void(bool)> completion,
                             int expected_backend_error, int test_accept_unsupported) {
    auto consumer = std::make_shared<Consumer>(
        std::move(config), std::move(executor), std::move(scheduler), std::move(ice), expect_turn,
        std::move(completion), expected_backend_error, test_accept_unsupported);
    std::weak_ptr<Consumer> weak = consumer;
    consumer->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
} // namespace p2pdc_test
