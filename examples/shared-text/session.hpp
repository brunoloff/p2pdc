#pragma once

#include "cursor.hpp"
#include "document.hpp"
#include "p2pdc/node.h"
#include "sync_state.hpp"
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <atomic>
#include <map>
#include <memory>

namespace shared_text {
class Session : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(EditorSession)
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(QString document READ document NOTIFY documentChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString route READ route NOTIFY routeChanged)
    Q_PROPERTY(QString cursorColor READ cursorColor NOTIFY cursorColorChanged)
    Q_PROPERTY(QVariantList remoteCursors READ remoteCursors NOTIFY remoteCursorsChanged)
    Q_PROPERTY(bool started READ started NOTIFY statusChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
  public:
    explicit Session(QObject *parent = nullptr);
    ~Session() override;
    [[nodiscard]] QString document() const;
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString route() const { return route_; }
    [[nodiscard]] QString cursorColor() const;
    [[nodiscard]] QVariantList remoteCursors() const;
    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] bool connected() const { return connected_; }
    Q_INVOKABLE QString newCapability() const;
    Q_INVOKABLE void start(const QString &capability, const QString &bootstrap, int port,
                           const QString &proxy, const QString &stunHost, int stunPort,
                           const QString &turnHost, int turnPort, const QString &turnUser,
                           const QString &turnPassword, const QString &turnTransport);
    Q_INVOKABLE void join();
    Q_INVOKABLE void edit(const QString &value);
    Q_INVOKABLE void setCursorPosition(int position);
    Q_INVOKABLE bool setCursorColor(const QString &color);
    Q_INVOKABLE void leave();
    Q_INVOKABLE void stop();
  signals:
    void documentChanged();
    void statusChanged();
    void routeChanged();
    void cursorColorChanged();
    void remoteCursorsChanged();

  private:
    p2pdc::Executor executor();
    p2pdc::Scheduler scheduler();
    void setStatus(QString value);
    void fail(const QString &reason);
    void nodeStarted(p2pdc::NodeStartResult result);
    bool watch();
    bool announce();
    using PeerKey = p2pdc::SigningPublicKey;
    struct Peer {
        std::optional<p2pdc::Advertisement> advertisement;
        std::unique_ptr<p2pdc::Connection> connection;
        std::uint64_t generation = 0;
        bool ready = false;
        bool dirty = true;
        PeerSync sync;
        p2pdc::RouteKind route = p2pdc::RouteKind::Unknown;
        std::optional<Cursor> cursor;
        std::chrono::steady_clock::time_point cursorSeen;
    };
    void tryConnect();
    bool listen();
    void connectPeer(const PeerKey &key, Peer &peer);
    void request(p2pdc::IncomingRequestId id, p2pdc::Advertisement peer);
    p2pdc::ConnectionHandlers handlers(PeerKey key, std::uint64_t generation);
    void connectionState(const PeerKey &key, std::uint64_t generation,
                         p2pdc::ConnectionState state);
    void receive(const PeerKey &key, std::uint64_t generation, p2pdc::Bytes message);
    void sendSnapshot(const PeerKey &key);
    void sendAll();
    void disconnectPeer(const PeerKey &key, std::uint64_t generation, const QString &reason);
    void updateStatus();
    void scheduleRetry();
    void scheduleCursor();
    void sendCursor();
    void receiveCursor(const PeerKey &key, p2pdc::ByteView message);
    void clearCursors();
    Document doc_;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    p2pdc::DiscoveryCapability capability_{};
    p2pdc::IceConfig ice_;
    std::unique_ptr<p2pdc::Node> node_;
    std::unique_ptr<p2pdc::Subscription> watch_;
    std::unique_ptr<p2pdc::Lease> lease_;
    std::unique_ptr<p2pdc::IncomingListener> listener_;
    std::map<PeerKey, Peer> peers_;
    std::uint64_t nextGeneration_ = 0;
    Cursor localCursor_{0, 0};
    QString status_ = QStringLiteral("Not connected"), route_ = QStringLiteral("Unknown");
    bool started_ = false, joined_ = false, published_ = false;
    bool caught_ = false, connected_ = false, stopping_ = false;
    std::uint64_t restartEpoch_ = 0;
};
} // namespace shared_text
