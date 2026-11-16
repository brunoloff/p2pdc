#include "session.hpp"

#include <QTimer>
#include <exception>

namespace shared_text {
using namespace p2pdc;
ConnectionHandlers Session::handlers(PeerKey key, std::uint64_t generation) {
    return {
        [this, key, generation](ConnectionState state) { connectionState(key, generation, state); },
        [this, key, generation](RouteInfo route) {
            auto it = peers_.find(key);
            if (it != peers_.end() && it->second.generation == generation) {
                it->second.route = route.current;
                updateStatus();
            }
        },
        [this, key, generation](Bytes value) { receive(key, generation, std::move(value)); },
        [this, key, generation] {
            auto it = peers_.find(key);
            if (it != peers_.end() && it->second.generation == generation)
                sendSnapshot(key);
        },
        [this, key, generation](ConnectError error) {
            disconnectPeer(key, generation,
                           QString("Connection failed (%1).").arg(static_cast<int>(error)));
        },
        [](ConnectionStatus) {}};
}
void Session::connectionState(const PeerKey &key, std::uint64_t generation, ConnectionState state) {
    auto it = peers_.find(key);
    if (it == peers_.end() || it->second.generation != generation)
        return;
    if (state == ConnectionState::Ready) {
        it->second.ready = true;
        it->second.sync.reset();
        it->second.dirty = true;
        updateStatus();
        sendSnapshot(key);
    } else if (state == ConnectionState::Suspended) {
        it->second.ready = false;
        updateStatus();
    } else if (state == ConnectionState::Failed || state == ConnectionState::Closed) {
        disconnectPeer(key, generation, "Connection closed.");
    }
}
void Session::disconnectPeer(const PeerKey &key, std::uint64_t generation, const QString &reason) {
    auto it = peers_.find(key);
    if (it == peers_.end() || it->second.generation != generation)
        return;
    it->second.ready = false;
    if (it->second.cursor) {
        it->second.cursor.reset();
        emit remoteCursorsChanged();
    }
    updateStatus();
    QTimer::singleShot(0, this, [this, key, generation, reason] {
        auto found = peers_.find(key);
        if (found == peers_.end() || found->second.generation != generation)
            return;
        found->second.connection.reset();
        found->second.route = RouteKind::Unknown;
        if (joined_)
            fail(reason + " Retrying…");
    });
}
void Session::updateStatus() {
    int count = 0;
    bool direct = false, relay = false;
    for (const auto &[key, peer] : peers_) {
        (void)key;
        if (!peer.ready)
            continue;
        ++count;
        direct |= peer.route == RouteKind::Direct;
        relay |= peer.route == RouteKind::TurnRelayed;
    }
    connected_ = count > 0;
    route_ = direct && relay ? "Mixed" : relay ? "TURN relay" : direct ? "Direct" : "Unknown";
    emit routeChanged();
    if (joined_)
        setStatus(count ? QString("Connected to %1 peer(s)").arg(count) : "Looking for peers…");
}
void Session::receive(const PeerKey &key, std::uint64_t generation, Bytes message) {
    auto it = peers_.find(key);
    if (it == peers_.end() || it->second.generation != generation)
        return;
    if (!message.empty() && message[0] == std::byte{2}) {
        receiveCursor(key, message);
        return;
    }
    if (message.empty() || message[0] != std::byte{1}) {
        disconnectPeer(key, generation, "Invalid document message.");
        return;
    }
    try {
        Document empty;
        const auto before = doc_.difference(empty.stateVector());
        if (!doc_.apply(ByteView(message).subspan(1))) {
            disconnectPeer(key, generation, "Invalid document update.");
            return;
        }
        emit documentChanged();
        if (before != doc_.difference(empty.stateVector())) {
            for (auto &[other, peer] : peers_)
                if (other != key)
                    peer.dirty = true;
            sendAll();
        }
    } catch (const std::exception &error) {
        disconnectPeer(key, generation, QString::fromUtf8(error.what()));
    }
}
void Session::sendAll() {
    for (auto &[key, peer] : peers_)
        if (peer.ready && peer.dirty)
            sendSnapshot(key);
}
void Session::sendSnapshot(const PeerKey &key) {
    auto it = peers_.find(key);
    if (it == peers_.end() || !it->second.dirty || !it->second.ready || !it->second.connection)
        return;
    try {
        auto update = it->second.sync.prepare(doc_);
        Bytes message{std::byte{1}};
        message.insert(message.end(), update.data.begin(), update.data.end());
        const auto result = it->second.connection->send(message);
        if (result == SendStatus::Accepted) {
            it->second.sync.accepted(std::move(update));
            it->second.dirty = false;
        } else if (result == SendStatus::TooLarge)
            fail("Document history exceeds the connection's message limit.");
        else if (result != SendStatus::Backpressure && result != SendStatus::NotReady)
            disconnectPeer(key, it->second.generation, "Could not send document.");
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
    }
}
} // namespace shared_text
