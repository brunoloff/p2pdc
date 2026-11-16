#include "node_race_consumer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>

namespace p2pdc_test {
using namespace p2pdc;
using namespace std::chrono_literals;
namespace {
struct Side {
    std::unique_ptr<Node> node;
    std::unique_ptr<Lease> lease;
    std::unique_ptr<Subscription> watch;
    std::unique_ptr<IncomingListener> listener;
    std::unique_ptr<Connection> outgoing, incoming;
    std::unique_ptr<Connection> extra_outgoing;
    std::optional<Advertisement> peer;
    bool published = false, caught = false, request = false;
};
class Race : public std::enable_shared_from_this<Race> {
  public:
    Race(DiscoveryConfig config, Executor executor, Scheduler scheduler, RaceMode mode,
         std::function<void(bool)> completion);
    void start();
    void cancel();

  private:
    void started(unsigned index, NodeStartResult result);
    bool watch(unsigned index);
    bool announce(unsigned index);
    void discover();
    bool listen(unsigned index);
    bool dial(unsigned index);
    void request(unsigned index, Advertisement peer);
    void persistent_request(IncomingRequestId id, Advertisement peer);
    void incoming_error(IncomingError error);
    void connection_error(ConnectError error);
    ConnectionHandlers handlers();
    void resolve_collision();
    void check_connection();
    void finish(bool success);
    void stopped(NodeStopResult result);
    std::array<Side, 2> sides_;
    Scheduler scheduler_;
    RaceMode mode_;
    std::function<void(bool)> completion_;
    std::shared_ptr<Race> keepalive_;
    CancelTimer deadline_;
    DiscoveryCapability capability_{};
    bool starting_ = false, resolved_ = false, sent_ = false, finishing_ = false, success_ = false;
    unsigned accepted_side_ = 0, stopped_ = 0;
    std::vector<IncomingRequestId> requests_;
};

Race::Race(DiscoveryConfig config, Executor executor, Scheduler scheduler, RaceMode mode,
           std::function<void(bool)> completion)
    : scheduler_(std::move(scheduler)), mode_(mode), completion_(std::move(completion)) {
    for (auto &side : sides_)
        side.node = Node::create(config, executor);
    capability_.fill(std::byte{static_cast<unsigned char>(73 + static_cast<unsigned>(mode))});
}
void Race::start() {
    keepalive_ = shared_from_this();
    auto weak = weak_from_this();
    deadline_ = scheduler_(25s, [weak] {
        if (auto self = weak.lock())
            self->finish(false);
    });
    for (unsigned i = 0; i < 2; ++i)
        if (!sides_[i].node->start([weak, i](NodeStartResult result) {
                if (auto self = weak.lock())
                    self->started(i, std::move(result));
            }))
            finish(false);
}
void Race::started(unsigned index, NodeStartResult result) {
    if (result.status != NodeStartStatus::Started || !watch(index) || !announce(index))
        finish(false);
}
bool Race::watch(unsigned index) {
    auto weak = weak_from_this();
    DiscoveryHandlers callbacks{
        [weak, index](Advertisement peer, PeerEvent event) {
            if (auto self = weak.lock(); self && event != PeerEvent::Expired) {
                auto identity = self->sides_[index].node->identity();
                if (identity && peer.signing_public_key != identity->signing_key) {
                    self->sides_[index].peer = std::move(peer);
                    self->discover();
                }
            }
        },
        [weak, index](bool ok) {
            if (auto self = weak.lock()) {
                self->sides_[index].caught = ok;
                if (ok)
                    self->discover();
                else
                    self->finish(false);
            }
        },
        [weak](WatchError) {
            if (auto self = weak.lock())
                self->finish(false);
        }};
    WatchOptions options;
    options.pulse_interval = 100ms;
    auto result = sides_[index].node->watch(capability_, scheduler_, options, std::move(callbacks));
    auto *handle = std::get_if<std::unique_ptr<Subscription>>(&result);
    if (!handle)
        return false;
    sides_[index].watch = std::move(*handle);
    return true;
}
bool Race::announce(unsigned index) {
    auto weak = weak_from_this();
    AnnounceHandlers callbacks{[weak, index] {
                                   if (auto self = weak.lock()) {
                                       self->sides_[index].published = true;
                                       self->discover();
                                   }
                               },
                               [weak](AnnouncementError error) {
                                   if (error != AnnouncementError::Retrying)
                                       if (auto self = weak.lock())
                                           self->finish(false);
                               }};
    AnnounceOptions options;
    options.pulse_interval = 100ms;
    auto result =
        sides_[index].node->announce(capability_, scheduler_, options, std::move(callbacks));
    auto *handle = std::get_if<std::unique_ptr<Lease>>(&result);
    if (!handle)
        return false;
    sides_[index].lease = std::move(*handle);
    return true;
}
void Race::discover() {
    if (finishing_ || starting_)
        return;
    for (const auto &side : sides_)
        if (!side.published || !side.caught || !side.peer || !side.lease || !side.watch)
            return;
    starting_ = true;
    if (!listen(1) || (mode_ == RaceMode::Collision && !listen(0)) || !dial(0) ||
        (mode_ == RaceMode::Collision && !dial(1))) {
        finish(false);
        return;
    }
    if (mode_ == RaceMode::Persistent) {
        auto &side = sides_[0];
        auto result = side.node->connect(*side.lease, *side.watch, *side.peer, {}, scheduler_, {},
                                         handlers());
        auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
        if (!connection) {
            finish(false);
            return;
        }
        side.extra_outgoing = std::move(*connection);
    }
}
bool Race::listen(unsigned index) {
    auto weak = weak_from_this();
    auto &side = sides_[index];
    IncomingOptions options;
    options.persistent = mode_ == RaceMode::Persistent;
    IncomingHandlers callbacks{[weak, index](Advertisement peer) {
                                   if (auto self = weak.lock())
                                       self->request(index, std::move(peer));
                               },
                               [weak](IncomingError error) {
                                   if (auto self = weak.lock())
                                       self->incoming_error(error);
                               }};
    callbacks.on_request_identified = [weak](IncomingRequestId id, Advertisement peer) {
        if (auto self = weak.lock())
            self->persistent_request(id, std::move(peer));
    };
    auto result =
        side.node->listen(*side.lease, *side.watch, scheduler_, options, std::move(callbacks));
    auto *handle = std::get_if<std::unique_ptr<IncomingListener>>(&result);
    if (!handle)
        return false;
    side.listener = std::move(*handle);
    return true;
}
void Race::persistent_request(IncomingRequestId id, Advertisement peer) {
    if (finishing_ || !sides_[1].peer || peer != *sides_[1].peer) {
        finish(false);
        return;
    }
    requests_.push_back(id);
    if (requests_.size() != 2)
        return;
    auto &side = sides_[1];
    auto invalid = side.node->accept(*side.listener, std::max(requests_[0], requests_[1]) + 1, {},
                                     scheduler_, {}, handlers());
    if (!std::holds_alternative<ConnectError>(invalid) ||
        std::get<ConnectError>(invalid) != ConnectError::InvalidPeer) {
        finish(false);
        return;
    }
    if (!side.listener->reject(requests_[0])) {
        finish(false);
        return;
    }
    auto result = side.node->accept(*side.listener, requests_[1], {}, scheduler_, {}, handlers());
    auto *connection = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!connection || side.listener->state() != IncomingState::Listening) {
        finish(false);
        return;
    }
    side.incoming = std::move(*connection);
    accepted_side_ = 1;
    check_connection();
}
bool Race::dial(unsigned index) {
    auto &side = sides_[index];
    auto result =
        side.node->connect(*side.lease, *side.watch, *side.peer, {}, scheduler_, {}, handlers());
    auto *handle = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!handle)
        return false;
    side.outgoing = std::move(*handle);
    return true;
}
ConnectionHandlers Race::handlers() {
    auto weak = weak_from_this();
    return {[](ConnectionState) {}, [](RouteInfo) {},
            [weak](Bytes data) {
                if (auto self = weak.lock())
                    self->finish(data == Bytes{std::byte{42}});
            },
            [] {},
            [weak](ConnectError error) {
                if (auto self = weak.lock())
                    self->connection_error(error);
            }};
}
void Race::request(unsigned index, Advertisement peer) {
    if (finishing_)
        return;
    if (!sides_[index].peer || peer != *sides_[index].peer) {
        finish(false);
        return;
    }
    sides_[index].request = true;
    if (mode_ == RaceMode::Collision) {
        resolve_collision();
        return;
    }
    if (mode_ == RaceMode::Cancel)
        sides_[0].outgoing->close();
    else if (mode_ == RaceMode::Reject)
        sides_[1].listener->close();
    else {
        auto &side = sides_[1];
        auto result = side.node->accept(*side.listener, {}, scheduler_, {}, handlers());
        auto *handle = std::get_if<std::unique_ptr<Connection>>(&result);
        if (!handle) {
            finish(false);
            return;
        }
        side.incoming = std::move(*handle);
        side.incoming->close();
    }
}
void Race::incoming_error(IncomingError error) {
    if (finishing_)
        return;
    if (mode_ == RaceMode::Cancel && error == IncomingError::Canceled)
        finish(true);
    else if (mode_ != RaceMode::Collision || error != IncomingError::Canceled) {
        std::cerr << "unexpected incoming error " << static_cast<int>(error) << '\n';
        finish(false);
    }
}
void Race::connection_error(ConnectError error) {
    if (finishing_)
        return;
    if (mode_ == RaceMode::Persistent && error == ConnectError::Rejected)
        return;
    if ((mode_ == RaceMode::Reject && error == ConnectError::Rejected) ||
        (mode_ == RaceMode::CancelAccepted && error == ConnectError::Canceled))
        finish(true);
    else if (mode_ != RaceMode::Collision ||
             (error != ConnectError::Superseded && error != ConnectError::Rejected &&
              error != ConnectError::Canceled)) {
        std::cerr << "unexpected connection error " << static_cast<int>(error) << '\n';
        finish(false);
    }
}
void Race::resolve_collision() {
    if (resolved_ || !sides_[0].request || !sides_[1].request)
        return;
    resolved_ = true;
    unsigned accepted = 0;
    for (unsigned i = 0; i < 2; ++i) {
        auto &side = sides_[i];
        auto result = side.node->accept(*side.listener, {}, scheduler_, {}, handlers());
        if (auto *handle = std::get_if<std::unique_ptr<Connection>>(&result)) {
            side.incoming = std::move(*handle);
            accepted_side_ = i;
            ++accepted;
        } else if (std::get<ConnectError>(result) != ConnectError::Superseded) {
            finish(false);
            return;
        }
    }
    if (accepted != 1) {
        finish(false);
        return;
    }
    check_connection();
}
void Race::check_connection() {
    if (finishing_ || sent_)
        return;
    auto &incoming = sides_[accepted_side_].incoming;
    auto &source = sides_[1 - accepted_side_];
    auto *outgoing = source.outgoing.get();
    if (mode_ == RaceMode::Persistent && source.extra_outgoing &&
        source.extra_outgoing->state() == ConnectionState::Ready)
        outgoing = source.extra_outgoing.get();
    if (incoming->state() == ConnectionState::Ready && outgoing &&
        outgoing->state() == ConnectionState::Ready) {
        sent_ = true;
        if (outgoing->send(Bytes{std::byte{42}}) != SendStatus::Accepted)
            finish(false);
        return;
    }
    auto weak = weak_from_this();
    (void)scheduler_(20ms, [weak] {
        if (auto self = weak.lock())
            self->check_connection();
    });
}
void Race::finish(bool success) {
    if (finishing_)
        return;
    if (!success) {
        std::cerr << "race failed: starting=" << starting_ << " resolved=" << resolved_
                  << " sent=" << sent_ << '\n';
        for (const auto &side : sides_)
            std::cerr << "  published=" << side.published << " caught=" << side.caught
                      << " peer=" << bool(side.peer) << " request=" << side.request << '\n';
    }
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
            stopped({NodeStopStatus::BackendFailure, {}});
}
void Race::stopped(NodeStopResult result) {
    success_ = success_ && result.status == NodeStopStatus::Stopped;
    if (++stopped_ == 2) {
        completion_(success_);
        keepalive_.reset();
    }
}
void Race::cancel() {
    if (!finishing_)
        finish(false);
}
} // namespace
std::function<void()> run_node_race_consumer(DiscoveryConfig config, Executor executor,
                                             Scheduler scheduler, RaceMode mode,
                                             std::function<void(bool)> completion) {
    auto test = std::make_shared<Race>(std::move(config), std::move(executor), std::move(scheduler),
                                       mode, std::move(completion));
    std::weak_ptr<Race> weak = test;
    test->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
} // namespace p2pdc_test
