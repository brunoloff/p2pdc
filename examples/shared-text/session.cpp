#include "session.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <sodium.h>

namespace shared_text {
using namespace p2pdc;
namespace {
template <typename T> bool parseHex(const QString &input, T &output) {
    const auto text = input.toLatin1();
    if (text.size() != static_cast<qsizetype>(output.size() * 2))
        return false;
    std::size_t size = 0;
    if (sodium_hex2bin(reinterpret_cast<unsigned char *>(output.data()), output.size(),
                       text.constData(), text.size(), nullptr, &size, nullptr) != 0)
        return false;
    return size == output.size() && std::any_of(output.begin(), output.end(),
                                                [](auto value) { return value != std::byte{}; });
}
IceConfig makeIce(const QString &stunHost, int stunPort, const QString &turnHost, int turnPort,
                  const QString &user, const QString &password, const QString &transportName) {
    IceConfig ice;
    if (!stunHost.isEmpty())
        ice.servers.emplace_back(
            StunServer{stunHost.toStdString(), static_cast<std::uint16_t>(stunPort)});
    if (!turnHost.isEmpty()) {
        const auto transport = transportName == "tls"   ? TurnTransport::Tls
                               : transportName == "tcp" ? TurnTransport::Tcp
                                                        : TurnTransport::Udp;
        ice.servers.emplace_back(TurnServer{turnHost.toStdString(),
                                            static_cast<std::uint16_t>(turnPort),
                                            user.toStdString(), password.toStdString(), transport});
    }
    return ice;
}
} // namespace
Session::Session(QObject *parent) : QObject(parent) {
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium initialization failed");
    constexpr std::array<std::uint32_t, 10> colors{0xc62828, 0x1565c0, 0x00838f, 0x6a1b9a,
                                                   0x2e7d32, 0xad1457, 0x4e548c, 0xbf360c,
                                                   0x00695c, 0x5d4037};
    localCursor_.color =
        colors[QRandomGenerator::global()->bounded(static_cast<quint32>(colors.size()))];
}
Session::~Session() {
    alive_->store(false);
    if (node_)
        node_.reset();
}
QString Session::document() const { return doc_.text(); }
QString Session::newCapability() const {
    std::array<unsigned char, capability_size> bytes{};
    std::array<char, capability_size * 2 + 1> hex{};
    randombytes_buf(bytes.data(), bytes.size());
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    sodium_memzero(bytes.data(), bytes.size());
    return QString::fromLatin1(hex.data());
}
Executor Session::executor() {
    auto *application = QCoreApplication::instance();
    auto alive = alive_;
    return [application, alive](std::function<void()> task) {
        if (application && alive->load())
            QMetaObject::invokeMethod(
                application,
                [alive, task = std::move(task)] {
                    if (alive->load())
                        task();
                },
                Qt::QueuedConnection);
    };
}
Scheduler Session::scheduler() {
    QPointer<Session> weak(this);
    return [weak](std::chrono::milliseconds delay, std::function<void()> task) {
        if (!weak)
            throw std::runtime_error("session has been destroyed");
        auto canceled = std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(delay, weak, [weak, canceled, task = std::move(task)] {
            if (weak && !canceled->load())
                task();
        });
        return [canceled] { canceled->store(true); };
    };
}
void Session::setStatus(QString value) {
    status_ = std::move(value);
    emit statusChanged();
}
void Session::fail(const QString &reason) { setStatus(reason); }
void Session::start(const QString &capability, const QString &bootstrap, int port,
                    const QString &proxy, const QString &stunHost, int stunPort,
                    const QString &turnHost, int turnPort, const QString &turnUser,
                    const QString &turnPassword, const QString &turnTransport) {
    if (node_ || !parseHex(capability, capability_) || port < 1 || port > 65535 ||
        (bootstrap.isEmpty() && proxy.isEmpty()) ||
        (!stunHost.isEmpty() && (stunPort < 1 || stunPort > 65535)) ||
        (!turnHost.isEmpty() && turnTransport != "udp" && turnTransport != "tcp" &&
         turnTransport != "tls") ||
        (!turnHost.isEmpty() &&
         (turnPort < 1 || turnPort > 65535 || turnUser.isEmpty() || turnPassword.isEmpty()))) {
        fail(QStringLiteral("Check the capability and network settings."));
        return;
    }
    DiscoveryConfig config;
    if (!bootstrap.isEmpty())
        config.bootstrap_servers.push_back(
            {bootstrap.toStdString(), static_cast<std::uint16_t>(port)});
    if (!proxy.isEmpty())
        config.browser_backends.push_back(p2pdc::OpenDhtProxies{{proxy.toStdString()}});
    ice_ = makeIce(stunHost, stunPort, turnHost, turnPort, turnUser, turnPassword, turnTransport);
    try {
        node_ = Node::create(std::move(config), executor());
        setStatus(QStringLiteral("Starting discovery…"));
        if (!node_->start([this](NodeStartResult result) { nodeStarted(std::move(result)); }))
            throw std::runtime_error("node did not start");
    } catch (const std::exception &error) {
        node_.reset();
        fail(QString::fromUtf8(error.what()));
    }
}
void Session::nodeStarted(NodeStartResult result) {
    if (result.status != NodeStartStatus::Started) {
        node_.reset();
        fail(QStringLiteral("Could not start discovery."));
        return;
    }
    if (!watch() || !announce() || !node_->identity()) {
        watch_.reset();
        lease_.reset();
        node_.reset();
        fail(QStringLiteral("Could not start discovery handles."));
        return;
    }
    started_ = true;
    setStatus(QStringLiteral("Ready to join."));
}
void Session::join() {
    if (!started_ || joined_)
        return;
    joined_ = true;
    setStatus(QStringLiteral("Looking for peers…"));
    tryConnect();
    scheduleRetry();
    scheduleCursor();
}
void Session::edit(const QString &value) {
    try {
        if (!doc_.replace(value)) {
            fail(QStringLiteral(
                "Text must be valid Unicode, without NUL, and at most 65,536 characters."));
            emit documentChanged();
            return;
        }
        for (auto &[key, peer] : peers_) {
            (void)key;
            peer.dirty = true;
        }
        sendAll();
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
        emit documentChanged();
    }
}
void Session::leave() {
    ++restartEpoch_;
    joined_ = connected_ = false;
    clearCursors();
    listener_.reset();
    for (auto &[key, peer] : peers_) {
        (void)key;
        peer.connection.reset();
        peer.ready = false;
    }
    route_ = "Unknown";
    emit routeChanged();
    setStatus(QStringLiteral("Left session; document remains available."));
}
void Session::stop() {
    if (!node_ || stopping_)
        return;
    ++restartEpoch_;
    stopping_ = true;
    joined_ = false;
    connected_ = false;
    clearCursors();
    listener_.reset();
    peers_.clear();
    watch_.reset();
    lease_.reset();
    setStatus(QStringLiteral("Stopping…"));
    if (!node_->shutdown([this](NodeStopResult result) {
            node_.reset();
            started_ = false;
            stopping_ = false;
            published_ = caught_ = false;
            setStatus(result.status == NodeStopStatus::Stopped ? QStringLiteral("Stopped")
                                                               : QStringLiteral("Shutdown failed"));
        })) {
        stopping_ = false;
        fail(QStringLiteral("Could not stop node."));
    }
}
} // namespace shared_text
