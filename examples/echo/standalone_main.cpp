#include "p2pdc/runtime.h"
#include "route_display.hpp"
#include "standalone_config.hpp"
#include "standalone_host.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
using namespace p2pdc;
using namespace p2pdc_echo;
using namespace std::chrono_literals;

Environment environment() {
    return [](std::string_view name) -> std::optional<std::string> {
        const std::string key(name);
        const char *value = std::getenv(key.c_str());
        return value ? std::optional<std::string>(value) : std::nullopt;
    };
}
int public_key(const Environment &read) {
    auto seed_text = read("P2PDC_ECHO_SEED");
    std::array<std::byte, 32> seed{};
    if (!seed_text || !parse_hex(*seed_text, seed) ||
        std::all_of(seed.begin(), seed.end(), [](std::byte byte) { return byte == std::byte{}; })) {
        std::cerr << "P2PDC_ECHO_SEED must be nonzero 32-byte hexadecimal\n";
        return 2;
    }
    auto result = Identity::import_seed(seed);
    auto *identity = std::get_if<Identity>(&result);
    if (!identity) {
        std::cerr << "could not derive signing public key\n";
        return 2;
    }
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : identity->public_key()) {
        const auto value = std::to_integer<unsigned>(byte);
        std::cout << digits[value >> 4] << digits[value & 15];
    }
    std::cout << std::endl;
    return 0;
}
class App : public std::enable_shared_from_this<App> {
  public:
    explicit App(StandaloneConfig config);
    void start();
    [[nodiscard]] std::shared_future<int> completion() const { return future_; }

  private:
    void begin();
    [[nodiscard]] Options options() const;
    [[nodiscard]] Callbacks callbacks();
    void route(RouteInfo value);
    void echoed();
    void failed(std::string reason);
    void finish(int status);
    void complete(int status);
    StandaloneConfig config_;
    std::shared_ptr<Runtime> runtime_ = std::make_shared<Runtime>();
    std::shared_ptr<EchoPeer> peer_;
    std::shared_ptr<App> keepalive_;
    std::promise<int> promise_;
    std::shared_future<int> future_ = promise_.get_future().share();
    CancelTimer timeout_;
    bool echoed_ = false, finishing_ = false;
};
App::App(StandaloneConfig config) : config_(std::move(config)) {}
void App::start() {
    auto self = shared_from_this();
    runtime_->executor()([self] { self->begin(); });
}
Options App::options() const {
    Options result;
    result.discovery = config_.discovery;
    result.capability = config_.capability;
    result.authorized_peer = config_.authorized_peer;
    result.signing_seed = config_.signing_seed;
    result.recipient_secret = config_.recipient_secret;
    result.ice = config_.ice;
    result.role = config_.role;
    return result;
}
Callbacks App::callbacks() {
    auto weak = weak_from_this();
    return {[weak](RouteInfo value) {
                if (auto self = weak.lock())
                    self->route(value);
            },
            [weak] {
                if (auto self = weak.lock())
                    self->echoed();
            },
            [weak](std::string reason) {
                if (auto self = weak.lock())
                    self->failed(std::move(reason));
            }};
}
void App::begin() {
    keepalive_ = shared_from_this();
    auto weak = weak_from_this();
    try {
        timeout_ = runtime_->scheduler()(60s, [weak] {
            if (auto self = weak.lock())
                self->failed("echo timed out");
        });
        auto result =
            EchoPeer::create(options(), runtime_->executor(), runtime_->scheduler(), callbacks());
        auto *peer = std::get_if<std::shared_ptr<EchoPeer>>(&result);
        if (!peer) {
            failed(std::get<std::string>(result));
            return;
        }
        peer_ = std::move(*peer);
        if (!peer_->start())
            failed("node start was rejected");
    } catch (const std::exception &error) {
        failed(std::string("could not start echo: ") + error.what());
    }
}
void App::route(RouteInfo value) {
    std::cout << "route: " << route_name(value.current);
    if (value.turn_history == TurnHistory::Used)
        std::cout << " (TURN used)";
    if (value.selected_pair)
        std::cout << " [local=" << candidate_name(value.selected_pair->local)
                  << ", remote=" << candidate_name(value.selected_pair->remote) << "]";
    std::cout << std::endl;
}
void App::echoed() {
    echoed_ = true;
    if (config_.role == Role::Initiator) {
        std::cout << "echo received" << std::endl;
        finish(0);
    } else {
        std::cout << "echo acknowledged; waiting for peer" << std::endl;
    }
}
void App::failed(std::string reason) {
    if (config_.role == Role::Responder && echoed_ && reason.starts_with("connection failed")) {
        std::cout << "peer disconnected after echo was acknowledged" << std::endl;
        finish(0);
        return;
    }
    std::cerr << "echo: " << reason << std::endl;
    finish(1);
}
void App::finish(int status) {
    if (finishing_)
        return;
    finishing_ = true;
    if (timeout_)
        timeout_();
    if (!peer_) {
        complete(status);
        return;
    }
    auto weak = weak_from_this();
    if (!peer_->shutdown([weak, status](bool stopped) {
            if (auto self = weak.lock())
                self->complete(stopped ? status : 1);
        }))
        complete(1);
}
void App::complete(int status) {
    host_complete(status);
    promise_.set_value(status);
    keepalive_.reset();
}
} // namespace

int main(int argc, char **argv) {
    const auto read = environment();
    if (argc == 2 && std::string_view(argv[1]) == "--public-key")
        return public_key(read);
    if (argc != 1) {
        std::cerr << "usage: p2pdc_echo [--public-key]\n";
        return 2;
    }
    auto parsed = parse_config(read);
    if (auto *error = std::get_if<std::string>(&parsed)) {
        std::cerr << "echo configuration: " << *error << std::endl;
        host_complete(2);
        return 2;
    }
    auto app = std::make_shared<App>(std::move(std::get<StandaloneConfig>(parsed)));
    app->start();
    return host_wait(app->completion());
}
