#include "session.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <opendht.h>

using namespace std::chrono_literals;
namespace {
bool until(const std::function<bool()> &predicate, std::chrono::seconds timeout = 25s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
            return true;
        QThread::msleep(2);
    }
    return predicate();
}
class SharedSessionTest : public ::testing::Test {
  protected:
    dht::DhtRunner bootstrap;
    void SetUp() override {
        dht::DhtRunner::Config config;
        config.peer_discovery = false;
        config.peer_publish = false;
        bootstrap.run("127.0.0.1", "::1", "0", config);
    }
    void TearDown() override { bootstrap.join(); }
    void start(shared_text::Session &session, const QString &capability) {
        session.start(capability, "127.0.0.1", bootstrap.getBoundPort(AF_INET), "", "", 3478, "",
                      3478, "", "", "udp");
    }
    void connectPeers(shared_text::Session &a, shared_text::Session &b) {
        a.join();
        b.join();
        ASSERT_TRUE(until([&] { return a.connected() && b.connected(); }))
            << a.status().toStdString() << " / " << b.status().toStdString();
    }
    void concurrentEdits(shared_text::Session &a, shared_text::Session &b,
                         shared_text::Session &c) {
        a.edit(a.document() + " Alpha");
        b.edit(b.document() + " Beta");
        c.edit(c.document() + " Gamma");
        ASSERT_TRUE(until([&] {
            return a.document() == b.document() && b.document() == c.document() &&
                   a.document().contains("Alpha") && a.document().contains("Beta") &&
                   a.document().contains("Gamma");
        })) << a.document().toStdString()
            << " / " << b.document().toStdString() << " / " << c.document().toStdString();
        a.edit("One replacement");
        b.edit("Another replacement");
        c.edit("Third replacement");
        ASSERT_TRUE(
            until([&] { return a.document() == b.document() && b.document() == c.document(); }));
    }
    void reconnect(shared_text::Session &a, shared_text::Session &b) {
        a.leave();
        ASSERT_TRUE(until([&] { return !b.connected(); }));
        a.edit(a.document() + " offline");
        a.join();
        ASSERT_TRUE(until([&] { return a.connected() && b.connected(); }))
            << a.status().toStdString() << " / " << b.status().toStdString();
        ASSERT_TRUE(until(
            [&] { return a.document() == b.document() && b.document().contains("offline"); }));
    }
};
TEST_F(SharedSessionTest, TwoEditorsMergeAndShutDown) {
    shared_text::Session a, b;
    const auto capability = a.newCapability();
    start(a, capability);
    start(b, capability);
    ASSERT_TRUE(until([&] { return a.started() && b.started(); }))
        << a.status().toStdString() << " / " << b.status().toStdString();
    a.edit("Left ");
    b.edit("Right ");
    ASSERT_NO_FATAL_FAILURE(connectPeers(a, b));
    ASSERT_TRUE(until([&] { return a.route() == "Direct" && b.route() == "Direct"; }));
    ASSERT_TRUE(until([&] {
        return a.document() == b.document() && a.document().contains("Left") &&
               a.document().contains("Right");
    }));
    auto first = a.document(), second = b.document();
    a.edit(first + " A");
    b.edit(second + " B");
    ASSERT_TRUE(until([&] {
        return a.document() == b.document() && a.document().contains(" A") &&
               a.document().contains(" B");
    }));
    ASSERT_NO_FATAL_FAILURE(reconnect(a, b));
    a.stop();
    b.stop();
    ASSERT_TRUE(until([&] { return !a.started() && !b.started(); }));
}
TEST_F(SharedSessionTest, ThreeEditorsConnectAndMergeWithoutExchangingKeys) {
    shared_text::Session a, b, c;
    const auto capability = a.newCapability();
    start(a, capability);
    start(b, capability);
    ASSERT_TRUE(until([&] { return a.started() && b.started(); }));
    a.edit("Before joining");
    a.join();
    b.join();
    ASSERT_TRUE(
        until([&] { return a.connected() && b.connected() && a.document() == b.document(); }));
    start(c, capability);
    ASSERT_TRUE(until([&] { return c.started(); }));
    c.join();
    ASSERT_TRUE(until([&] { return c.connected() && c.document() == a.document(); }));
    ASSERT_TRUE(a.setCursorColor("#112233"));
    ASSERT_TRUE(b.setCursorColor("#445566"));
    ASSERT_TRUE(c.setCursorColor("#778899"));
    ASSERT_TRUE(until([&] {
        return a.remoteCursors().size() == 2 && b.remoteCursors().size() == 2 &&
               c.remoteCursors().size() == 2;
    }));
    ASSERT_NO_FATAL_FAILURE(concurrentEdits(a, b, c));
    b.edit("Deleted everything else");
    ASSERT_TRUE(
        until([&] { return a.document() == b.document() && b.document() == c.document(); }));
    a.leave();
    b.edit(b.document() + " remaining");
    ASSERT_TRUE(until([&] { return c.document().contains("remaining"); }));
    a.join();
    ASSERT_TRUE(
        until([&] { return a.document() == b.document() && b.document() == c.document(); }));
    a.stop();
    b.stop();
    c.stop();
    ASSERT_TRUE(until([&] { return !a.started() && !b.started() && !c.started(); }));
}
TEST_F(SharedSessionTest, CursorPresenceUpdatesAndClearsWhenPeerLeaves) {
    shared_text::Session a, b;
    const auto capability = a.newCapability();
    start(a, capability);
    start(b, capability);
    ASSERT_TRUE(until([&] { return a.started() && b.started(); }));
    a.edit("Hello!");
    a.setCursorPosition(3);
    a.setCursorPosition(-1);
    a.setCursorPosition(65537);
    ASSERT_TRUE(a.setCursorColor("#12AB34"));
    EXPECT_EQ(a.cursorColor(), "#12ab34");
    EXPECT_FALSE(a.setCursorColor("#+12345"));
    EXPECT_FALSE(a.setCursorColor("#123"));
    ASSERT_NO_FATAL_FAILURE(connectPeers(a, b));
    ASSERT_TRUE(until([&] { return b.remoteCursors().size() == 1; }));
    auto first = b.remoteCursors().front().toMap();
    EXPECT_EQ(first.value("position").toUInt(), 3U);
    EXPECT_EQ(first.value("color").toString(), "#12ab34");
    a.setCursorPosition(5);
    ASSERT_TRUE(a.setCursorColor("#abcdef"));
    ASSERT_TRUE(until([&] {
        const auto cursors = b.remoteCursors();
        return cursors.size() == 1 && cursors.front().toMap().value("position").toUInt() == 5 &&
               cursors.front().toMap().value("color").toString() == "#abcdef";
    }));
    a.leave();
    ASSERT_TRUE(until([&] { return b.remoteCursors().empty(); }));
    a.stop();
    b.stop();
    ASSERT_TRUE(until([&] { return !a.started() && !b.started(); }));
}
TEST_F(SharedSessionTest, RejectsInvalidCapability) {
    shared_text::Session session;
    start(session, "invalid");
    EXPECT_FALSE(session.started());
    EXPECT_TRUE(session.status().contains("capability"));
    const auto capability = session.newCapability();
    EXPECT_EQ(capability.size(), 64);
    session.start(capability, "127.0.0.1", bootstrap.getBoundPort(AF_INET), "", "", 3478,
                  "turn.invalid", 3478, "user", "password", "invalid");
    EXPECT_FALSE(session.started());
    EXPECT_TRUE(session.status().contains("network"));
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
