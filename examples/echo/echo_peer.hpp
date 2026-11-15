#pragma once

#include "echo_protocol.hpp"
#include "p2pdc/node.h"

#include <memory>
#include <string>
#include <variant>

namespace p2pdc_echo {
struct Options {
    p2pdc::DiscoveryConfig discovery;
    p2pdc::DiscoveryCapability capability{};
    p2pdc::SigningPublicKey authorized_peer{};
    p2pdc::ByteView signing_seed;
    p2pdc::ByteView recipient_secret;
    p2pdc::IceConfig ice;
    Role role = Role::Initiator;
};
struct Callbacks {
    std::function<void(p2pdc::RouteInfo)> on_route;
    std::function<void()> on_echo;
    std::function<void(std::string)> on_error;
};

// Call on the supplied executor and retain the peer until shutdown completes.
// The caller supplies a high-entropy capability and an independently verified
// signing key for the remote peer. Seed and secret views are borrowed only by
// create(); the caller protects and wipes them. Callbacks must not throw.
class EchoPeer : public std::enable_shared_from_this<EchoPeer> {
  public:
    static std::variant<std::shared_ptr<EchoPeer>, std::string> create(Options options,
                                                                       p2pdc::Executor executor,
                                                                       p2pdc::Scheduler scheduler,
                                                                       Callbacks callbacks);
    [[nodiscard]] bool start();
    [[nodiscard]] bool shutdown(std::function<void(bool)> completion);

  private:
    EchoPeer(Options options, p2pdc::Scheduler scheduler, Callbacks callbacks,
             std::unique_ptr<p2pdc::Node> node);
    void started(p2pdc::NodeStartResult result);
    [[nodiscard]] bool watch();
    [[nodiscard]] bool announce();
    void try_connect();
    [[nodiscard]] bool listen();
    [[nodiscard]] bool connect();
    void request(p2pdc::Advertisement peer);
    [[nodiscard]] p2pdc::ConnectionHandlers connection_handlers();
    void state(p2pdc::ConnectionState value);
    void message(p2pdc::Bytes value);
    void fail(std::string reason);
    Options options_;
    p2pdc::Scheduler scheduler_;
    Callbacks callbacks_;
    EchoProtocol protocol_;
    std::unique_ptr<p2pdc::Node> node_;
    std::unique_ptr<p2pdc::Subscription> watch_;
    std::unique_ptr<p2pdc::Lease> lease_;
    std::unique_ptr<p2pdc::IncomingListener> listener_;
    std::unique_ptr<p2pdc::Connection> connection_;
    std::optional<p2pdc::Advertisement> peer_;
    bool published_ = false, caught_ = false, connecting_ = false;
    bool sent_ = false, failed_ = false, stopping_ = false;
};
[[nodiscard]] const char *route_name(p2pdc::RouteKind value);
} // namespace p2pdc_echo
