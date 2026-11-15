#include "node_mixed_race.hpp"

#include <chrono>
#include <iostream>

namespace p2pdc_test {
using namespace p2pdc;
using namespace std::chrono_literals;
namespace {
class MixedPeer : public std::enable_shared_from_this<MixedPeer> {
  public:
    MixedPeer(DiscoveryConfig config, Executor executor, Scheduler scheduler, MixedRace mode,
              bool offerer, std::function<void(bool)> completion);
    void start();
    void cancel();

  private:
    void started(NodeStartResult result);
    bool watch();
    bool announce();
    void discover();
    bool listen();
    bool dial();
    void cancel_dial();
    void request(Advertisement peer);
    void incoming_error(IncomingError error);
    void connection_error(ConnectError error);
    ConnectionHandlers handlers();
    void message(Bytes value);
    void check();
    void delay_success();
    void finish(bool success);
    std::unique_ptr<Node> node_;
    std::unique_ptr<Lease> lease_;
    std::unique_ptr<Subscription> watch_;
    std::unique_ptr<IncomingListener> listener_;
    std::unique_ptr<Connection> outgoing_, incoming_;
    Connection *selected_ = nullptr;
    std::optional<Advertisement> peer_;
    Scheduler scheduler_;
    MixedRace mode_;
    bool offerer_;
    std::function<void(bool)> completion_;
    std::shared_ptr<MixedPeer> keepalive_;
    DiscoveryCapability capability_{};
    CancelTimer deadline_;
    bool published_ = false, caught_ = false, started_ = false, finishing_ = false;
    bool sent_ = false, received_ = false, acknowledged_ = false, success_scheduled_ = false;
    bool request_seen_ = false;
};
MixedPeer::MixedPeer(DiscoveryConfig config, Executor executor, Scheduler scheduler, MixedRace mode,
                     bool offerer, std::function<void(bool)> completion)
    : node_(Node::create(std::move(config), std::move(executor))), scheduler_(std::move(scheduler)),
      mode_(mode), offerer_(offerer), completion_(std::move(completion)) {
    capability_.fill(std::byte{74});
}
void MixedPeer::start() {
    keepalive_ = shared_from_this();
    auto weak = weak_from_this();
    deadline_ = scheduler_(35s, [weak] {
        if (auto self = weak.lock())
            self->finish(false);
    });
    if (!node_->start([weak](NodeStartResult result) {
            if (auto self = weak.lock())
                self->started(std::move(result));
        }))
        finish(false);
}
void MixedPeer::started(NodeStartResult result) {
    if (result.status != NodeStartStatus::Started || !watch() || !announce())
        finish(false);
}
bool MixedPeer::watch() {
    auto weak = weak_from_this();
    DiscoveryHandlers callbacks{
        [weak](Advertisement peer, PeerEvent event) {
            if (auto self = weak.lock(); self && event != PeerEvent::Expired) {
                auto identity = self->node_->identity();
                if (identity && peer.signing_public_key != identity->signing_key) {
                    self->peer_ = std::move(peer);
                    self->discover();
                }
            }
        },
        [weak](bool ok) {
            if (auto self = weak.lock()) {
                self->caught_ = ok;
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
    auto result = node_->watch(capability_, scheduler_, options, std::move(callbacks));
    auto *handle = std::get_if<std::unique_ptr<Subscription>>(&result);
    if (!handle)
        return false;
    watch_ = std::move(*handle);
    return true;
}
bool MixedPeer::announce() {
    auto weak = weak_from_this();
    AnnounceOptions options;
    options.pulse_interval = 100ms;
    auto result = node_->announce(capability_, scheduler_, options,
                                  {[weak] {
                                       if (auto self = weak.lock()) {
                                           self->published_ = true;
                                           self->discover();
                                       }
                                   },
                                   [weak](AnnouncementError error) {
                                       if (error != AnnouncementError::Retrying)
                                           if (auto self = weak.lock())
                                               self->finish(false);
                                   }});
    auto *handle = std::get_if<std::unique_ptr<Lease>>(&result);
    if (!handle)
        return false;
    lease_ = std::move(*handle);
    return true;
}
void MixedPeer::discover() {
    if (finishing_ || started_ || !published_ || !caught_ || !peer_ || !lease_ || !watch_)
        return;
    started_ = true;
    if ((!offerer_ || mode_ == MixedRace::Collision) && !listen()) {
        finish(false);
        return;
    }
    if ((offerer_ || mode_ == MixedRace::Collision) && !dial())
        finish(false);
}
bool MixedPeer::listen() {
    auto weak = weak_from_this();
    auto result = node_->listen(*lease_, *watch_, scheduler_, {},
                                {[weak](Advertisement peer) {
                                     if (auto self = weak.lock())
                                         self->request(std::move(peer));
                                 },
                                 [weak](IncomingError error) {
                                     if (auto self = weak.lock())
                                         self->incoming_error(error);
                                 }});
    auto *handle = std::get_if<std::unique_ptr<IncomingListener>>(&result);
    if (!handle)
        return false;
    listener_ = std::move(*handle);
    return true;
}
bool MixedPeer::dial() {
    auto result = node_->connect(*lease_, *watch_, *peer_, {}, scheduler_, {}, handlers());
    auto *handle = std::get_if<std::unique_ptr<Connection>>(&result);
    if (!handle)
        return false;
    outgoing_ = std::move(*handle);
    if (mode_ == MixedRace::CancelDial)
        cancel_dial();
    return true;
}
void MixedPeer::cancel_dial() {
    auto weak = weak_from_this();
    (void)scheduler_(3s, [weak] {
        if (auto self = weak.lock(); self && !self->finishing_) {
            self->outgoing_->close();
            self->delay_success();
        }
    });
}
ConnectionHandlers MixedPeer::handlers() {
    auto weak = weak_from_this();
    return {[](ConnectionState) {}, [](RouteInfo) {},
            [weak](Bytes value) {
                if (auto self = weak.lock())
                    self->message(std::move(value));
            },
            [] {},
            [weak](ConnectError error) {
                if (auto self = weak.lock())
                    self->connection_error(error);
            }};
}
void MixedPeer::request(Advertisement peer) {
    if (finishing_)
        return;
    if (!peer_ || peer != *peer_) {
        finish(false);
        return;
    }
    request_seen_ = true;
    if (mode_ == MixedRace::CancelDial)
        return;
    if (mode_ == MixedRace::Reject) {
        listener_->close();
        delay_success();
        return;
    }
    auto result = node_->accept(*listener_, {}, scheduler_, {}, handlers());
    if (auto *handle = std::get_if<std::unique_ptr<Connection>>(&result)) {
        incoming_ = std::move(*handle);
        if (mode_ == MixedRace::CancelAccepted) {
            incoming_->close();
            delay_success();
            return;
        }
        selected_ = incoming_.get();
    } else if (mode_ == MixedRace::Collision &&
               std::get<ConnectError>(result) == ConnectError::Superseded) {
        selected_ = outgoing_.get();
    } else {
        finish(false);
        return;
    }
    check();
}
void MixedPeer::incoming_error(IncomingError error) {
    if (finishing_)
        return;
    std::cerr << "mixed incoming error " << static_cast<int>(error) << '\n';
    if (mode_ == MixedRace::CancelDial && request_seen_ && error == IncomingError::Canceled) {
        delay_success();
    } else if (mode_ == MixedRace::Collision && error == IncomingError::Canceled && outgoing_) {
        selected_ = outgoing_.get();
        check();
    } else
        finish(false);
}
void MixedPeer::connection_error(ConnectError error) {
    if (finishing_)
        return;
    std::cerr << "mixed connection error " << static_cast<int>(error) << '\n';
    if (offerer_ && ((mode_ == MixedRace::Reject && error == ConnectError::Rejected) ||
                     (mode_ == MixedRace::CancelAccepted && error == ConnectError::Canceled))) {
        delay_success();
    } else if (mode_ != MixedRace::Collision ||
               (error != ConnectError::Superseded && error != ConnectError::Rejected &&
                error != ConnectError::Canceled))
        finish(false);
}
void MixedPeer::message(Bytes value) {
    if (finishing_ || mode_ != MixedRace::Collision)
        return;
    if (!selected_ && outgoing_ && outgoing_->state() == ConnectionState::Ready)
        selected_ = outgoing_.get();
    if (!selected_) {
        finish(false);
        return;
    }
    if (value == Bytes{std::byte{42}}) {
        received_ = true;
        if (selected_->send(Bytes{std::byte{43}}) != SendStatus::Accepted) {
            finish(false);
            return;
        }
    } else if (value == Bytes{std::byte{43}})
        acknowledged_ = true;
    else {
        finish(false);
        return;
    }
    check();
}
void MixedPeer::check() {
    if (finishing_ || !selected_)
        return;
    if (selected_->state() == ConnectionState::Failed ||
        selected_->state() == ConnectionState::Closed) {
        finish(false);
        return;
    }
    if (selected_->state() == ConnectionState::Ready && !sent_) {
        sent_ = true;
        if (selected_->send(Bytes{std::byte{42}}) != SendStatus::Accepted) {
            finish(false);
            return;
        }
    }
    if (sent_ && received_ && acknowledged_) {
        delay_success();
        return;
    }
    auto weak = weak_from_this();
    (void)scheduler_(25ms, [weak] {
        if (auto self = weak.lock())
            self->check();
    });
}
void MixedPeer::delay_success() {
    if (success_scheduled_)
        return;
    success_scheduled_ = true;
    auto weak = weak_from_this();
    (void)scheduler_(1s, [weak] {
        if (auto self = weak.lock())
            self->finish(true);
    });
}
void MixedPeer::finish(bool success) {
    if (finishing_)
        return;
    if (!success) {
        std::cerr << "mixed failure: published=" << published_ << " caught=" << caught_
                  << " peer=" << bool(peer_) << " started=" << started_
                  << " selected=" << bool(selected_) << " sent=" << sent_
                  << " received=" << received_ << " acknowledged=" << acknowledged_ << '\n';
    }
    finishing_ = true;
    if (deadline_)
        deadline_();
    auto weak = weak_from_this();
    if (!node_->shutdown([weak, success](NodeStopResult result) {
            if (auto self = weak.lock()) {
                self->completion_(success && result.status == NodeStopStatus::Stopped);
                self->keepalive_.reset();
            }
        })) {
        completion_(false);
        keepalive_.reset();
    }
}
void MixedPeer::cancel() {
    if (!finishing_)
        finish(false);
}
} // namespace
std::function<void()> run_mixed_race(DiscoveryConfig config, Executor executor, Scheduler scheduler,
                                     MixedRace mode, bool offerer,
                                     std::function<void(bool)> completion) {
    auto peer =
        std::make_shared<MixedPeer>(std::move(config), std::move(executor), std::move(scheduler),
                                    mode, offerer, std::move(completion));
    std::weak_ptr<MixedPeer> weak = peer;
    peer->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
} // namespace p2pdc_test
