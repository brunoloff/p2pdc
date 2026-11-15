#pragma once

#include "../src/discovery/record_store.hpp"
#include "p2pdc/node.h"
#include "p2pdc/signaling.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace p2pdc_test {
using namespace p2pdc;
using namespace std::chrono_literals;

inline std::unique_ptr<Node> test_node(DiscoveryConfig config, Executor executor) {
    auto created = Node::create_with_identity(std::move(config), std::move(executor),
                                              Bytes(32, std::byte{3}), Bytes(32, std::byte{4}));
    if (auto node = std::get_if<std::unique_ptr<Node>>(&created))
        return std::move(*node);
    throw std::runtime_error("test identity creation failed");
}

class NodeDiscoveryConsumer : public std::enable_shared_from_this<NodeDiscoveryConsumer> {
  public:
    NodeDiscoveryConsumer(DiscoveryConfig config, Executor executor, Scheduler scheduler,
                          detail::RecordStore store, std::function<void(bool)> completion)
        : node_(test_node(std::move(config), std::move(executor))), store_(std::move(store)),
          scheduler_(std::move(scheduler)), completion_(std::move(completion)) {}
    void start();
    void cancel();

  private:
    void started(NodeStartResult result);
    bool watch();
    bool announce();
    bool listen();
    bool send_offer();
    void requested(Advertisement peer);
    void check();
    void finish(bool success);
    void stopped(NodeStopResult result);
    std::unique_ptr<Node> node_;
    detail::RecordStore store_;
    Scheduler scheduler_;
    std::function<void(bool)> completion_;
    std::unique_ptr<Subscription> watch_;
    std::unique_ptr<Lease> lease_;
    std::unique_ptr<IncomingListener> listener_;
    std::shared_ptr<NodeDiscoveryConsumer> keepalive_;
    DiscoveryCapability capability_{};
    std::optional<Advertisement> peer_;
    bool caught_ = false, seen_ = false, published_ = false;
    bool finishing_ = false, success_ = false;
};

inline void NodeDiscoveryConsumer::start() {
    keepalive_ = shared_from_this();
    capability_.fill(std::byte{9});
    auto weak = weak_from_this();
    if (!node_->start([weak](NodeStartResult result) {
            if (auto self = weak.lock())
                self->started(std::move(result));
        }))
        finish(false);
}
inline void NodeDiscoveryConsumer::started(NodeStartResult result) {
    if (result.status != NodeStartStatus::Started || !watch() || !announce())
        finish(false);
}
inline bool NodeDiscoveryConsumer::watch() {
    auto weak = weak_from_this();
    DiscoveryHandlers handlers{[weak](Advertisement peer, PeerEvent event) {
                                   if (auto self = weak.lock(); self && event == PeerEvent::Seen) {
                                       self->peer_ = std::move(peer);
                                       self->seen_ = true;
                                       self->check();
                                   }
                               },
                               [weak](bool ok) {
                                   if (auto self = weak.lock()) {
                                       self->caught_ = ok;
                                       if (ok)
                                           self->check();
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
    auto result = node_->watch(capability_, scheduler_, options, std::move(handlers));
    if (auto value = std::get_if<std::unique_ptr<Subscription>>(&result)) {
        watch_ = std::move(*value);
        return true;
    }
    return false;
}
inline bool NodeDiscoveryConsumer::announce() {
    auto weak = weak_from_this();
    AnnounceHandlers handlers{[weak] {
                                  if (auto self = weak.lock()) {
                                      self->published_ = true;
                                      self->check();
                                  }
                              },
                              [weak](AnnouncementError) {
                                  if (auto self = weak.lock())
                                      self->finish(false);
                              }};
    AnnounceOptions options;
    options.pulse_interval = 100ms;
    auto result = node_->announce(capability_, scheduler_, options, std::move(handlers));
    if (auto value = std::get_if<std::unique_ptr<Lease>>(&result)) {
        lease_ = std::move(*value);
        return true;
    }
    return false;
}
inline void NodeDiscoveryConsumer::check() {
    if (!listener_ && caught_ && seen_ && published_ && watch_ && lease_) {
        if (!node_->healthy() || !watch_->healthy() || !lease_->healthy() || !node_->identity() ||
            !listen() || !send_offer())
            finish(false);
    }
}
inline bool NodeDiscoveryConsumer::listen() {
    auto weak = weak_from_this();
    IncomingHandlers handlers{[weak](Advertisement peer) {
                                  if (auto self = weak.lock())
                                      self->requested(std::move(peer));
                              },
                              [weak](IncomingError error) {
                                  if (auto self = weak.lock()) {
                                      std::cerr << "incoming error " << static_cast<int>(error)
                                                << '\n';
                                      self->finish(false);
                                  }
                              }};
    auto result = node_->listen(*lease_, *watch_, scheduler_, {}, handlers);
    auto value = std::get_if<std::unique_ptr<IncomingListener>>(&result);
    if (!value)
        return false;
    listener_ = std::move(*value);
    auto duplicate = node_->listen(*lease_, *watch_, scheduler_, {}, std::move(handlers));
    return listener_->state() == IncomingState::Listening &&
           std::get_if<IncomingError>(&duplicate) &&
           std::get<IncomingError>(duplicate) == IncomingError::ResourceLimit;
}
inline bool NodeDiscoveryConsumer::send_offer() {
    if (!peer_)
        return false;
    auto identity = Identity::import_seed(Bytes(32, std::byte{3}));
    if (!std::holds_alternative<Identity>(identity))
        return false;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (now < 0)
        return false;
    const auto issued = static_cast<std::uint64_t>(now);
    SignalingFields fields{SignalingKind::Offer,
                           peer_->fields.instance_id,
                           {peer_->signing_public_key, peer_->fields.instance_id},
                           AttemptId{std::byte{7}},
                           0,
                           issued,
                           issued + 30,
                           Bytes{std::byte{1}}};
    auto encoded =
        encode_signaling(std::get<Identity>(identity), fields, peer_->fields.recipient_public_key,
                         capability_, {issued, 60, 5, 32768});
    auto wire = std::get_if<Bytes>(&encoded);
    if (!wire)
        return false;
    auto tampered = *wire;
    tampered.back() ^= std::byte{1};
    auto ignore = [](bool ok) {
        if (!ok)
            std::cerr << "offer put failed\n";
    };
    return !store_.put(peer_->fields.signaling_inbox, tampered, ignore) &&
           !store_.put(peer_->fields.signaling_inbox, *wire, ignore);
}
inline void NodeDiscoveryConsumer::requested(Advertisement peer) {
    finish(peer_ && peer == *peer_ && listener_->state() == IncomingState::Pending &&
           listener_->peer() == peer_);
}
inline void NodeDiscoveryConsumer::finish(bool success) {
    if (finishing_)
        return;
    finishing_ = true;
    success_ = success;
    auto weak = weak_from_this();
    if (!node_->shutdown([weak](NodeStopResult result) {
            if (auto self = weak.lock())
                self->stopped(std::move(result));
        }))
        throw std::logic_error("test node shutdown already started");
}
inline void NodeDiscoveryConsumer::stopped(NodeStopResult result) {
    const bool success = success_ && result.status == NodeStopStatus::Stopped &&
                         !watch_->healthy() && !lease_->healthy() &&
                         listener_->state() == IncomingState::Closed && !node_->identity();
    completion_(success);
    keepalive_.reset();
}
inline void NodeDiscoveryConsumer::cancel() {
    finishing_ = true;
    node_.reset();
    watch_.reset();
    lease_.reset();
    listener_.reset();
    keepalive_.reset();
}
inline std::function<void()> run_node_discovery_consumer(DiscoveryConfig config, Executor executor,
                                                         Scheduler scheduler,
                                                         detail::RecordStore store,
                                                         std::function<void(bool)> completion) {
    auto consumer = std::make_shared<NodeDiscoveryConsumer>(std::move(config), std::move(executor),
                                                            std::move(scheduler), std::move(store),
                                                            std::move(completion));
    std::weak_ptr<NodeDiscoveryConsumer> weak = consumer;
    consumer->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
} // namespace p2pdc_test
