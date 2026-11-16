#include "session.hpp"

#include <QTimer>

namespace shared_text {
using namespace p2pdc;
QString Session::cursorColor() const {
    return QStringLiteral("#%1").arg(localCursor_.color, 6, 16, QLatin1Char('0'));
}
QVariantList Session::remoteCursors() const {
    QVariantList result;
    for (const auto &[key, peer] : peers_) {
        if (!peer.ready || !peer.cursor)
            continue;
        const auto id = QByteArray(reinterpret_cast<const char *>(key.data()), key.size()).toHex();
        const auto color = QStringLiteral("#%1").arg(peer.cursor->color, 6, 16, QLatin1Char('0'));
        result.push_back(QVariantMap{{"id", QString::fromLatin1(id)},
                                     {"position", peer.cursor->position},
                                     {"color", color}});
    }
    return result;
}
void Session::setCursorPosition(int position) {
    if (position >= 0 && position <= 65536)
        localCursor_.position = static_cast<std::uint32_t>(position);
}
bool Session::setCursorColor(const QString &color) {
    if (color.size() != 7 || color[0] != QLatin1Char('#'))
        return false;
    for (int i = 1; i < color.size(); ++i) {
        const auto c = color[i].toLower();
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    }
    bool ok = false;
    const auto value = color.mid(1).toUInt(&ok, 16);
    if (!ok || value > 0xffffff)
        return false;
    if (localCursor_.color != value) {
        localCursor_.color = value;
        emit cursorColorChanged();
    }
    return true;
}
void Session::sendCursor() {
    const auto message = encodeCursor(localCursor_);
    const auto now = std::chrono::steady_clock::now();
    bool expired = false;
    for (auto &[key, peer] : peers_) {
        (void)key;
        if (peer.cursor && now - peer.cursorSeen > std::chrono::seconds(5)) {
            peer.cursor.reset();
            expired = true;
        }
        if (peer.ready && peer.connection)
            (void)peer.connection->send(message);
    }
    if (expired)
        emit remoteCursorsChanged();
}
void Session::scheduleCursor() {
    const auto epoch = restartEpoch_;
    QTimer::singleShot(std::chrono::seconds(1), this, [this, epoch] {
        if (!joined_ || stopping_ || epoch != restartEpoch_)
            return;
        sendCursor();
        scheduleCursor();
    });
}
void Session::receiveCursor(const PeerKey &key, ByteView message) {
    auto cursor = decodeCursor(message);
    auto peer = peers_.find(key);
    if (!cursor || peer == peers_.end() || !peer->second.ready)
        return;
    const bool changed = !peer->second.cursor ||
                         peer->second.cursor->position != cursor->position ||
                         peer->second.cursor->color != cursor->color;
    peer->second.cursor = *cursor;
    peer->second.cursorSeen = std::chrono::steady_clock::now();
    if (changed)
        emit remoteCursorsChanged();
}
void Session::clearCursors() {
    bool changed = false;
    for (auto &[key, peer] : peers_) {
        (void)key;
        changed |= peer.cursor.has_value();
        peer.cursor.reset();
    }
    if (changed)
        emit remoteCursorsChanged();
}
} // namespace shared_text
