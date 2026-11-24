#include "../src/browser/browser_record_store.hpp"
#include "node_discovery_consumer.hpp"
#include "p2pdc/recipient_crypto.h"
#include "p2pdc/runtime.h"

using namespace p2pdc;

namespace {
bool rejects_invalid_configuration(Executor executor) {
    auto rejects = [&executor](DiscoveryConfig config) {
        try {
            (void)Node::create(std::move(config), executor);
            return false;
        } catch (const std::invalid_argument &) {
            return true;
        }
    };
    DiscoveryConfig insecure, mixed, too_many, capacity;
    insecure.browser_backends = {OpenDhtProxies{{"http://example.com"}}};
    mixed.browser_backends = {OpenDhtProxies{{"https://one.example", "http://two.example"}}};
    too_many.browser_backends = std::vector<BrowserBackend>(9, NostrRelay{"wss://relay.example"});
    capacity.browser_backends = {OpenDhtProxies{{"http://127.0.0.1:49412"}}};
    capacity.max_connections = 0;
    if (!rejects(capacity))
        return false;
    capacity.max_connections = 1025;
    if (!rejects(capacity))
        return false;
    capacity.max_connections = 10;
    auto valid = Node::create(capacity, executor);
    DiscoveryConfig several;
    several.browser_backends = {OpenDhtProxies{{"https://one.example", "https://two.example"}},
                                NostrRelay{"wss://relay.example"},
                                MqttBroker{"wss://broker.example/mqtt"},
                                PeerJsServer{"wss://peers.example/peerjs"}};
    auto failover = Node::create(std::move(several), executor);
    return valid && failover && rejects({}) && rejects(std::move(insecure)) &&
           rejects(std::move(mixed)) && rejects(std::move(too_many));
}
bool imports_identity(Executor executor) {
    DiscoveryConfig config;
    config.browser_backends = {OpenDhtProxies{{"http://127.0.0.1:49412"}}};
    Bytes signing(32, std::byte{3}), recipient(32, std::byte{4});
    auto bad = Node::create_with_identity(config, executor, {}, recipient);
    if (!std::holds_alternative<CryptoError>(bad))
        return false;
    auto created = Node::create_with_identity(config, executor, signing, recipient);
    auto node = std::get_if<std::unique_ptr<Node>>(&created);
    auto expected_signing = Identity::import_seed(signing);
    auto expected_recipient = RecipientKeyPair::import_secret_key(recipient);
    if (!node || !std::holds_alternative<Identity>(expected_signing) ||
        !std::holds_alternative<RecipientKeyPair>(expected_recipient) || !(*node)->identity())
        return false;
    const auto actual = *(*node)->identity();
    return actual.signing_key == std::get<Identity>(expected_signing).public_key() &&
           actual.recipient_key == std::get<RecipientKeyPair>(expected_recipient).public_key();
}
struct BrowserProbe : std::enable_shared_from_this<BrowserProbe> {
    BrowserProbe(Runtime &runtime, std::function<void(bool)> done)
        : runtime(runtime), done(std::move(done)) {}
    Runtime &runtime;
    std::function<void(bool)> done;
    std::shared_ptr<BrowserProbe> keepalive;
    std::unique_ptr<Node> node;
    std::function<void()> cancel_consumer;
    bool canceled_start = false;
    bool finished = false;
    void start();
    void stopped(NodeStopResult result);
    void finish(bool result);
    void cancel();
    DiscoveryConfig config() const {
        DiscoveryConfig value;
        value.browser_backends = {OpenDhtProxies{{"http://127.0.0.1:49412"}}};
        return value;
    }
};
void BrowserProbe::start() {
    keepalive = shared_from_this();
    if (!rejects_invalid_configuration(runtime.executor()) ||
        !imports_identity(runtime.executor())) {
        finish(false);
        return;
    }
    node = Node::create(config(), runtime.executor());
    auto weak = weak_from_this();
    if (!node->start([weak](NodeStartResult result) {
            if (auto self = weak.lock())
                self->canceled_start = result.status == NodeStartStatus::Canceled;
        }) ||
        !node->shutdown([weak](NodeStopResult result) {
            if (auto self = weak.lock())
                self->stopped(std::move(result));
        }))
        finish(false);
}
void BrowserProbe::stopped(NodeStopResult result) {
    if (!canceled_start || result.status != NodeStopStatus::Stopped) {
        finish(false);
        return;
    }
    node.reset();
    auto weak = weak_from_this();
    cancel_consumer = p2pdc_test::run_node_discovery_consumer(
        config(), runtime.executor(), runtime.scheduler(),
        detail::make_browser_record_store("http://127.0.0.1:49412", {32768, 128, 1048576}),
        [weak](bool success) {
            if (auto self = weak.lock())
                self->finish(success);
        });
}
void BrowserProbe::finish(bool result) {
    if (finished)
        return;
    finished = true;
    done(result);
    keepalive.reset();
}
void BrowserProbe::cancel() {
    if (finished)
        return;
    finished = true;
    if (cancel_consumer)
        cancel_consumer();
    node.reset();
    keepalive.reset();
}
} // namespace

std::function<void()> start_browser_public_discovery(Runtime &runtime,
                                                     std::function<void(bool)> completion) {
    auto probe = std::make_shared<BrowserProbe>(runtime, std::move(completion));
    std::weak_ptr<BrowserProbe> weak = probe;
    probe->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
