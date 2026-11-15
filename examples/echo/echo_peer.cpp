#include "echo_peer.hpp"

#include <algorithm>
#include <utility>

namespace p2pdc_echo {
using namespace p2pdc;
namespace {
template <typename Range> bool all_zero(const Range &value) {
    return std::all_of(value.begin(), value.end(),
                       [](std::byte byte) { return byte == std::byte{}; });
}
} // namespace

std::variant<std::shared_ptr<EchoPeer>, std::string>
EchoPeer::create(Options options, Executor executor, Scheduler scheduler, Callbacks callbacks) {
    if (!executor || !scheduler || !callbacks.on_route || !callbacks.on_echo ||
        !callbacks.on_error || all_zero(options.capability) || all_zero(options.authorized_peer) ||
        (options.role != Role::Initiator && options.role != Role::Responder))
        return std::string("invalid echo options or callbacks");
    auto result = Node::create_with_identity(options.discovery, std::move(executor),
                                             options.signing_seed, options.recipient_secret);
    auto *node = std::get_if<std::unique_ptr<Node>>(&result);
    if (!node)
        return std::string("could not create node with the supplied identity");
    options.signing_seed = {};
    options.recipient_secret = {};
    return std::shared_ptr<EchoPeer>(new EchoPeer(std::move(options), std::move(scheduler),
                                                  std::move(callbacks), std::move(*node)));
}
EchoPeer::EchoPeer(Options options, Scheduler scheduler, Callbacks callbacks,
                   std::unique_ptr<Node> node)
    : options_(std::move(options)), scheduler_(std::move(scheduler)),
      callbacks_(std::move(callbacks)), protocol_(options_.role), node_(std::move(node)) {}
bool EchoPeer::start() {
    auto weak = weak_from_this();
    return node_->start([weak](NodeStartResult result) {
        if (auto self = weak.lock())
            self->started(std::move(result));
    });
}
bool EchoPeer::shutdown(std::function<void(bool)> completion) {
    if (stopping_ || !completion)
        return false;
    stopping_ = true;
    auto self = shared_from_this();
    return node_->shutdown([self, completion = std::move(completion)](NodeStopResult result) {
        completion(result.status == NodeStopStatus::Stopped);
    });
}
void EchoPeer::started(NodeStartResult result) {
    if (stopping_)
        return;
    if (result.status != NodeStartStatus::Started || !watch() || !announce())
        fail("node startup or discovery admission failed");
}
bool EchoPeer::watch() {
    auto weak = weak_from_this();
    DiscoveryHandlers callbacks{[weak](Advertisement peer, PeerEvent event) {
                                    if (auto self = weak.lock();
                                        self && event != PeerEvent::Expired &&
                                        peer.signing_public_key == self->options_.authorized_peer) {
                                        self->peer_ = std::move(peer);
                                        self->try_connect();
                                    }
                                },
                                [weak](bool success) {
                                    if (auto self = weak.lock()) {
                                        self->caught_ = success;
                                        if (success)
                                            self->try_connect();
                                        else
                                            self->fail("discovery catch-up failed");
                                    }
                                },
                                [weak](WatchError) {
                                    if (auto self = weak.lock())
                                        self->fail("discovery failed");
                                }};
    auto result = node_->watch(options_.capability, scheduler_, {}, std::move(callbacks));
    auto *watch = std::get_if<std::unique_ptr<Subscription>>(&result);
    if (!watch)
        return false;
    watch_ = std::move(*watch);
    return true;
}
bool EchoPeer::announce() {
    auto weak = weak_from_this();
    AnnounceHandlers callbacks{[weak] {
                                   if (auto self = weak.lock()) {
                                       self->published_ = true;
                                       self->try_connect();
                                   }
                               },
                               [weak](AnnouncementError error) {
                                   if (error != AnnouncementError::Retrying)
                                       if (auto self = weak.lock())
                                           self->fail("announcement failed");
                               }};
    auto result = node_->announce(options_.capability, scheduler_, {}, std::move(callbacks));
    auto *lease = std::get_if<std::unique_ptr<Lease>>(&result);
    if (!lease)
        return false;
    lease_ = std::move(*lease);
    return true;
}
void EchoPeer::try_connect() {
    if (failed_ || stopping_ || connecting_ || !published_ || !caught_ || !watch_ || !lease_)
        return;
    if (options_.role == Role::Responder) {
        connecting_ = true;
        if (!listen())
            fail("listener admission failed");
    } else if (peer_ && watch_->is_current(*peer_)) {
        connecting_ = true;
        if (!connect())
            fail("connection admission failed");
    }
}
bool EchoPeer::listen() {
    auto weak = weak_from_this();
    auto result = node_->listen(*lease_, *watch_, scheduler_, {},
                                {[weak](Advertisement peer) {
                                     if (auto self = weak.lock())
                                         self->request(std::move(peer));
                                 },
                                 [weak](IncomingError) {
                                     if (auto self = weak.lock())
                                         self->fail("incoming listener failed");
                                 }});
    auto *listener = std::get_if<std::unique_ptr<IncomingListener>>(&result);
    if (!listener)
        return false;
    listener_ = std::move(*listener);
    return true;
}
bool EchoPeer::connect() {
    auto result = node_->connect(*lease_, *watch_, *peer_, options_.ice, scheduler_, {},
                                 connection_handlers());
    auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!connection)
        return false;
    connection_ = std::move(*connection);
    return true;
}
void EchoPeer::request(Advertisement peer) {
    if (peer.signing_public_key != options_.authorized_peer || !watch_->is_current(peer)) {
        fail("incoming peer is not authorized or no longer current");
        return;
    }
    auto result = node_->accept(*listener_, options_.ice, scheduler_, {}, connection_handlers());
    auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!connection) {
        fail("incoming connection admission failed");
        return;
    }
    connection_ = std::move(*connection);
}
ConnectionHandlers EchoPeer::connection_handlers() {
    auto weak = weak_from_this();
    return {[weak](ConnectionState value) {
                if (auto self = weak.lock())
                    self->state(value);
            },
            [weak](RouteInfo value) {
                if (auto self = weak.lock(); self && !self->stopping_ && !self->failed_)
                    self->callbacks_.on_route(value);
            },
            [weak](Bytes value) {
                if (auto self = weak.lock())
                    self->message(std::move(value));
            },
            [] {},
            [weak](ConnectError error) {
                if (auto self = weak.lock())
                    self->fail("connection failed (code " +
                               std::to_string(static_cast<int>(error)) + ")");
            },
            {}};
}
void EchoPeer::state(ConnectionState value) {
    if (stopping_ || failed_)
        return;
    if (value == ConnectionState::Ready && options_.role == Role::Initiator && !sent_) {
        if (connection_->send(echo_message) != SendStatus::Accepted)
            fail("could not queue the echo message");
        else
            sent_ = true;
    }
}
void EchoPeer::message(Bytes value) {
    if (stopping_ || failed_)
        return;
    const auto action = protocol_.receive(value);
    if (action == EchoAction::Invalid) {
        fail("unexpected echo message");
        return;
    }
    if (action == EchoAction::Complete) {
        callbacks_.on_echo();
        return;
    }
    ByteView response;
    switch (action) {
    case EchoAction::SendEcho:
        response = echo_message;
        break;
    case EchoAction::SendAck:
        response = echo_ack;
        break;
    case EchoAction::SendConfirmation:
        response = echo_confirmation;
        break;
    default:
        fail("invalid echo protocol state");
        return;
    }
    if (connection_->send(response) != SendStatus::Accepted) {
        fail("could not queue the echo protocol message");
        return;
    }
    if (action == EchoAction::SendConfirmation)
        callbacks_.on_echo();
}
void EchoPeer::fail(std::string reason) {
    if (failed_ || stopping_)
        return;
    failed_ = true;
    callbacks_.on_error(std::move(reason));
}
const char *route_name(RouteKind value) {
    switch (value) {
    case RouteKind::Unknown:
        return "Unknown";
    case RouteKind::Direct:
        return "Direct";
    case RouteKind::TurnRelayed:
        return "TurnRelayed";
    }
    return "Unknown";
}
} // namespace p2pdc_echo
