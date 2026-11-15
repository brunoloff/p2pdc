#include "../src/browser/browser_mailbox_attempt.hpp"
#include "p2pdc/runtime.h"

#include <chrono>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class Watch final : public RecordWatchBackend {
  public:
    explicit Watch(std::shared_ptr<bool> closed, std::size_t token)
        : closed_(std::move(closed)), registration_(ready_.get_future().share()) {
        ready_.set_value(token);
    }
    ~Watch() override { cancel(); }
    void cancel() override { *closed_ = true; }
    std::shared_future<std::size_t> registration() const override { return registration_; }

  private:
    std::shared_ptr<bool> closed_;
    std::promise<std::size_t> ready_;
    std::shared_future<std::size_t> registration_;
};
class Store final : public RecordStoreBackend {
  public:
    std::shared_ptr<bool> closed = std::make_shared<bool>(false);
    std::optional<StoreError> lookup_error;
    std::size_t token = 1;
    std::optional<StoreError> put(ByteView, ByteView,
                                  std::function<void(bool)> callback) const override {
        callback(true);
        return std::nullopt;
    }
    std::optional<StoreError> lookup(ByteView, std::function<void(LookupResult)> callback,
                                     LookupDeduplication) const override {
        if (lookup_error)
            return lookup_error;
        callback(LookupResult{{}, true});
        return std::nullopt;
    }
    std::variant<RecordWatch, StoreError> watch(ByteView, std::function<void(StoreBatch)>,
                                                bool) const override {
        return RecordWatch(std::make_unique<Watch>(closed, token));
    }
};
struct Probe : std::enable_shared_from_this<Probe> {
    Probe(Runtime &runtime, std::function<void(bool)> done)
        : runtime(runtime), done(std::move(done)) {}
    Runtime &runtime;
    std::function<void(bool)> done;
    std::shared_ptr<Probe> keepalive;
    std::shared_ptr<Store> store = std::make_shared<Store>();
    std::unique_ptr<BrowserMailboxAttempt> attempt;
    CancelTimer timer;
    bool completed = false, failure_called = false;
    BrowserMailboxConfig config() const;
    bool startup_outcome(std::optional<StoreError> error, std::size_t token,
                         std::optional<AttemptFailure> expected);
    void start();
    void finish(bool success);
    void cancel();
};
BrowserMailboxConfig Probe::config() const {
    auto identity = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    auto box = std::make_shared<RecipientKeyPair>(
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{2}))));
    auto capability = std::make_shared<Bytes>(32, std::byte{3});
    AsyncSignalingBinding binding{{identity->public_key(), {std::byte{1}}},
                                  {identity->public_key(), {std::byte{2}}},
                                  box->public_key(),
                                  box->public_key(),
                                  {std::byte{7}},
                                  HandshakeRole::Answerer};
    BrowserSecureConfig secure{binding, identity, box, capability, {}, 1024, [] { return 100U; }};
    return {RecordStore(store), {std::byte{1}},        {std::byte{2}},
            std::move(secure),  MemoryBudget(4194304), Bytes{std::byte{1}}};
}
bool Probe::startup_outcome(std::optional<StoreError> error, std::size_t token,
                            std::optional<AttemptFailure> expected) {
    auto fake = std::make_shared<Store>();
    fake->lookup_error = error;
    fake->token = token;
    auto value = config();
    value.store = RecordStore(fake);
    value.accepted_offer.reset();
    BrowserMailboxCallbacks handlers{[](std::uint32_t) {},
                                     [](Bytes) {},
                                     [] {},
                                     [](RouteInfo) {},
                                     [](BrowserTransportState) {},
                                     [] {},
                                     [](AttemptFailure) {}};
    auto created = BrowserMailboxAttempt::create(std::move(value), runtime.executor(), handlers);
    if (!std::holds_alternative<std::unique_ptr<BrowserMailboxAttempt>>(created))
        return false;
    auto candidate = std::move(std::get<std::unique_ptr<BrowserMailboxAttempt>>(created));
    const auto result = candidate->poll_start();
    return expected
               ? result == BrowserMailboxStart::Failed && candidate->failure_reason() == *expected
               : result == BrowserMailboxStart::Started;
}
void Probe::start() {
    keepalive = shared_from_this();
    if (!startup_outcome({}, 1, {}) ||
        !startup_outcome(StoreError::ResourceLimit, 1, AttemptFailure::Resource) ||
        !startup_outcome(StoreError::NotRunning, 1, AttemptFailure::Signaling) ||
        !startup_outcome({}, 0, AttemptFailure::Signaling)) {
        finish(false);
        return;
    }
    Executor rejected = [](std::function<void()>) {
        throw std::runtime_error("test executor rejection");
    };
    auto weak = weak_from_this();
    BrowserMailboxCallbacks handlers{[](std::uint32_t) {},
                                     [](Bytes) {},
                                     [] {},
                                     [](RouteInfo) {},
                                     [](BrowserTransportState) {},
                                     [] {},
                                     [weak](AttemptFailure) {
                                         if (auto self = weak.lock())
                                             self->failure_called = true;
                                     }};
    auto created =
        BrowserMailboxAttempt::create(config(), std::move(rejected), std::move(handlers));
    if (!std::holds_alternative<std::unique_ptr<BrowserMailboxAttempt>>(created)) {
        finish(false);
        return;
    }
    attempt = std::move(std::get<std::unique_ptr<BrowserMailboxAttempt>>(created));
    if (attempt->poll_start() != BrowserMailboxStart::Failed) {
        finish(false);
        return;
    }
    timer = runtime.scheduler()(30ms, [weak] {
        if (auto self = weak.lock())
            self->finish(*self->store->closed && !self->failure_called && !self->attempt->ready() &&
                         self->attempt->send({}) == ChannelSend::Failed);
    });
}
void Probe::finish(bool success) {
    if (completed)
        return;
    completed = true;
    if (timer)
        timer();
    if (attempt)
        attempt->close();
    done(success);
    keepalive.reset();
}
void Probe::cancel() {
    if (completed)
        return;
    completed = true;
    if (timer)
        timer();
    if (attempt)
        attempt->close();
    keepalive.reset();
}
} // namespace
std::function<void()> run_browser_mailbox_failure_smoke(Runtime &runtime,
                                                        std::function<void(bool)> done) {
    auto probe = std::make_shared<Probe>(runtime, std::move(done));
    std::weak_ptr<Probe> weak = probe;
    probe->start();
    return [weak] {
        if (auto active = weak.lock())
            active->cancel();
    };
}
