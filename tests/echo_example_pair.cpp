#include "echo_example_pair.hpp"

#include <array>
#include <chrono>
#include <utility>

namespace p2pdc_test {
using namespace p2pdc;
using namespace p2pdc_echo;
using namespace std::chrono_literals;
namespace {
class Pair : public std::enable_shared_from_this<Pair> {
  public:
    Pair(DiscoveryConfig config, Executor executor, Scheduler scheduler, IceConfig ice,
         bool expect_turn, bool reject_peer, std::function<void(bool, std::string)> completion);
    void start();
    void finish(bool success, std::string reason);

  private:
    bool make_peers();
    bool make_peer(std::size_t index);
    void route(std::size_t index, RouteInfo value);
    void echoed(std::size_t index);
    void check();
    void stopped(bool success);
    DiscoveryConfig config_;
    Executor executor_;
    Scheduler scheduler_;
    IceConfig ice_;
    bool expect_turn_, reject_peer_;
    std::function<void(bool, std::string)> completion_;
    std::shared_ptr<Pair> keepalive_;
    std::array<std::shared_ptr<EchoPeer>, 2> peers_;
    std::array<bool, 2> echoed_{}, routed_{};
    std::array<Bytes, 2> seeds_{Bytes(32, std::byte{1}), Bytes(32, std::byte{2})};
    std::array<Bytes, 2> secrets_{Bytes(32, std::byte{3}), Bytes(32, std::byte{4})};
    std::array<SigningPublicKey, 2> keys_{};
    CancelTimer deadline_;
    unsigned pending_ = 0;
    bool finishing_ = false, success_ = false;
    std::string reason_;
};
Pair::Pair(DiscoveryConfig config, Executor executor, Scheduler scheduler, IceConfig ice,
           bool expect_turn, bool reject_peer, std::function<void(bool, std::string)> completion)
    : config_(std::move(config)), executor_(std::move(executor)), scheduler_(std::move(scheduler)),
      ice_(std::move(ice)), expect_turn_(expect_turn), reject_peer_(reject_peer),
      completion_(std::move(completion)) {}
void Pair::start() {
    keepalive_ = shared_from_this();
    auto weak = weak_from_this();
    deadline_ = scheduler_(30s, [weak] {
        if (auto self = weak.lock())
            self->finish(false, "echo timed out");
    });
    if (!make_peers()) {
        finish(false, "echo peer creation failed");
        return;
    }
    for (auto &peer : peers_)
        if (!peer->start()) {
            finish(false, "echo peer start rejected");
            return;
        }
}
bool Pair::make_peers() {
    for (std::size_t index = 0; index < keys_.size(); ++index) {
        auto result = Identity::import_seed(seeds_[index]);
        auto *identity = std::get_if<Identity>(&result);
        if (!identity)
            return false;
        keys_[index] = identity->public_key();
    }
    return make_peer(0) && make_peer(1);
}
bool Pair::make_peer(std::size_t index) {
    Options options;
    options.discovery = config_;
    options.capability.fill(std::byte{5});
    options.authorized_peer = keys_[1 - index];
    if (reject_peer_ && index == 1)
        options.authorized_peer[0] ^= std::byte{1};
    options.signing_seed = seeds_[index];
    options.recipient_secret = secrets_[index];
    options.ice = ice_;
    options.role = index == 0 ? Role::Initiator : Role::Responder;
    auto weak = weak_from_this();
    auto result = EchoPeer::create(std::move(options), executor_, scheduler_,
                                   {[weak, index](RouteInfo value) {
                                        if (auto self = weak.lock())
                                            self->route(index, value);
                                    },
                                    [weak, index] {
                                        if (auto self = weak.lock())
                                            self->echoed(index);
                                    },
                                    [weak](std::string reason) {
                                        if (auto self = weak.lock())
                                            self->finish(false, std::move(reason));
                                    }});
    auto *peer = std::get_if<std::shared_ptr<EchoPeer>>(&result);
    if (!peer)
        return false;
    peers_[index] = std::move(*peer);
    return true;
}
void Pair::route(std::size_t index, RouteInfo value) {
    routed_[index] = expect_turn_ ? value.current == RouteKind::TurnRelayed &&
                                        value.turn_history == TurnHistory::Used
                                  : value.current == RouteKind::Direct;
    check();
}
void Pair::echoed(std::size_t index) {
    echoed_[index] = true;
    check();
}
void Pair::check() {
    if (echoed_[0] && echoed_[1] && routed_[0] && routed_[1])
        finish(true, {});
}
void Pair::finish(bool success, std::string reason) {
    if (finishing_)
        return;
    finishing_ = true;
    success_ = success;
    reason_ = std::move(reason);
    if (deadline_)
        deadline_();
    auto weak = weak_from_this();
    for (auto &peer : peers_) {
        if (!peer)
            continue;
        if (peer->shutdown([weak](bool stopped) {
                if (auto self = weak.lock())
                    self->stopped(stopped);
            }))
            ++pending_;
        else
            success_ = false;
    }
    if (!pending_) {
        completion_(success_, reason_);
        keepalive_.reset();
    }
}
void Pair::stopped(bool success) {
    success_ = success_ && success;
    if (--pending_ == 0) {
        completion_(success_, reason_);
        keepalive_.reset();
    }
}
} // namespace

std::function<void()> run_echo_example_pair(DiscoveryConfig config, Executor executor,
                                            Scheduler scheduler, IceConfig ice, bool expect_turn,
                                            bool reject_peer,
                                            std::function<void(bool, std::string)> completion) {
    auto pair =
        std::make_shared<Pair>(std::move(config), std::move(executor), std::move(scheduler),
                               std::move(ice), expect_turn, reject_peer, std::move(completion));
    pair->start();
    return [weak = std::weak_ptr<Pair>(pair)] {
        if (auto current = weak.lock())
            current->finish(false, "echo canceled");
    };
}
} // namespace p2pdc_test
