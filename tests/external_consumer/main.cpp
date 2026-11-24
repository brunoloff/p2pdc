#include <p2pdc/node.h>
#include <p2pdc/runtime.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <variant>

int main() {
    p2pdc::Runtime runtime;
    p2pdc::DiscoveryConfig config;
    config.bootstrap_servers.push_back({"127.0.0.1", 4222});
    config.browser_backends.push_back(p2pdc::OpenDhtProxies{{"http://127.0.0.1:4223"}});
    std::array<std::byte, 32> signing_seed{}, recipient_secret{};
    signing_seed.fill(std::byte{1});
    recipient_secret.fill(std::byte{2});
    auto invalid =
        p2pdc::Node::create_with_identity(config, runtime.executor(), {}, recipient_secret);
    if (!std::holds_alternative<p2pdc::CryptoError>(invalid))
        return 1;
    auto result = p2pdc::Node::create_with_identity(config, runtime.executor(), signing_seed,
                                                    recipient_secret);
    auto *node = std::get_if<std::unique_ptr<p2pdc::Node>>(&result);
    if (!node || !*node || !(*node)->identity())
        return 2;
    std::cout << "external consumer passed\n";
    return 0;
}
