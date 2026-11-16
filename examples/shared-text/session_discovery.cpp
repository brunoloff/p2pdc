#include "session.hpp"

#include <QTimer>

namespace shared_text {
using namespace p2pdc;
bool Session::watch() {
    auto result = node_->watch(
        capability_, scheduler(), {},
        {[this](Advertisement value, PeerEvent event) {
             if (!node_->identity() || value.signing_public_key == node_->identity()->signing_key)
                 return;
             auto &peer = peers_[value.signing_public_key];
             if (event == PeerEvent::Expired) {
                 if (peer.advertisement &&
                     peer.advertisement->fields.instance_id == value.fields.instance_id)
                     peer.advertisement.reset();
             } else {
                 peer.advertisement = std::move(value);
                 tryConnect();
             }
         },
         [this](bool ok) {
             caught_ = ok;
             if (ok)
                 tryConnect();
             else
                 fail("Discovery catch-up failed.");
         },
         [this](WatchError error) {
             fail(QString("Discovery failed (%1).").arg(static_cast<int>(error)));
         }});
    auto *value = std::get_if<std::unique_ptr<Subscription>>(&result);
    if (!value)
        return false;
    watch_ = std::move(*value);
    return true;
}
bool Session::announce() {
    auto result = node_->announce(
        capability_, scheduler(), {},
        {[this] {
             published_ = true;
             tryConnect();
         },
         [this](AnnouncementError error) {
             if (error != AnnouncementError::Retrying)
                 fail(QString("Announcement failed (%1).").arg(static_cast<int>(error)));
         }});
    auto *value = std::get_if<std::unique_ptr<Lease>>(&result);
    if (!value)
        return false;
    lease_ = std::move(*value);
    return true;
}
void Session::tryConnect() {
    if (!joined_ || stopping_ || !published_ || !caught_ || !watch_ || !lease_ ||
        !node_->identity())
        return;
    if (!listener_ && !listen()) {
        fail("Listener unavailable; retrying…");
        return;
    }
    const auto own = node_->identity()->signing_key;
    for (auto &[key, peer] : peers_)
        if (own < key && !peer.connection && peer.advertisement &&
            watch_->is_current(*peer.advertisement))
            connectPeer(key, peer);
}
bool Session::listen() {
    IncomingOptions options;
    options.persistent = true;
    IncomingHandlers callbacks;
    callbacks.on_error = [this](IncomingError) {
        fail("Listener failed; retrying…");
        auto *failed = listener_.get();
        const auto epoch = restartEpoch_;
        QTimer::singleShot(0, this, [this, failed, epoch] {
            if (epoch == restartEpoch_ && listener_.get() == failed)
                listener_.reset();
        });
    };
    callbacks.on_request_identified = [this](IncomingRequestId id, Advertisement peer) {
        request(id, std::move(peer));
    };
    callbacks.on_overflow = [this] { fail("Too many incoming requests; retrying…"); };
    auto result = node_->listen(*lease_, *watch_, scheduler(), options, std::move(callbacks));
    auto *value = std::get_if<std::unique_ptr<IncomingListener>>(&result);
    if (!value)
        return false;
    listener_ = std::move(*value);
    updateStatus();
    return true;
}
void Session::connectPeer(const PeerKey &key, Peer &peer) {
    const auto generation = ++nextGeneration_;
    auto result = node_->connect(*lease_, *watch_, *peer.advertisement, ice_, scheduler(), {},
                                 handlers(key, generation));
    if (auto *connection = std::get_if<std::unique_ptr<Connection>>(&result)) {
        peer.generation = generation;
        peer.connection = std::move(*connection);
    } else if (std::get<ConnectError>(result) == ConnectError::Unsupported) {
        fail("WebRTC is unavailable on this platform.");
    }
}
void Session::request(IncomingRequestId id, Advertisement advertisement) {
    const auto key = advertisement.signing_public_key;
    if (!joined_ || !watch_->is_current(advertisement) || !node_->identity() ||
        key >= node_->identity()->signing_key) {
        (void)listener_->reject(id);
        return;
    }
    auto &peer = peers_[key];
    if (peer.connection) {
        (void)listener_->reject(id);
        return;
    }
    const auto generation = ++nextGeneration_;
    auto result = node_->accept(*listener_, id, ice_, scheduler(), {}, handlers(key, generation));
    if (auto *connection = std::get_if<std::unique_ptr<Connection>>(&result)) {
        peer.advertisement = std::move(advertisement);
        peer.generation = generation;
        peer.connection = std::move(*connection);
    } else {
        (void)listener_->reject(id);
    }
}
void Session::scheduleRetry() {
    const auto epoch = restartEpoch_;
    QTimer::singleShot(std::chrono::milliseconds(1500), this, [this, epoch] {
        if (!joined_ || stopping_ || epoch != restartEpoch_)
            return;
        tryConnect();
        scheduleRetry();
    });
}
} // namespace shared_text
