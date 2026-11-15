#include "../src/discovery/native_discovery_cycle.hpp"
#include "../src/discovery/native_public_watch.hpp"

#include <deque>
#include <map>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
struct WatchState {
    std::promise<std::size_t> ready;
    std::shared_future<std::size_t> future = ready.get_future().share();
    std::function<void(StoreBatch)> callback;
    bool active = true;
};
class Watch final : public RecordWatchBackend {
  public:
    explicit Watch(std::shared_ptr<WatchState> state) : state_(std::move(state)) {}
    ~Watch() override { cancel(); }
    void cancel() override { state_->active = false; }
    std::shared_future<std::size_t> registration() const override { return state_->future; }

  private:
    std::shared_ptr<WatchState> state_;
};
class Store final : public RecordStoreBackend {
  public:
    mutable std::map<Bytes, std::vector<Bytes>> records;
    mutable std::map<Bytes, std::shared_ptr<WatchState>> watches;
    bool fail_lookup = false;
    std::optional<StoreError> put(ByteView key, ByteView payload,
                                  std::function<void(bool)> completion) const override;
    std::optional<StoreError> lookup(ByteView key, std::function<void(LookupResult)> completion,
                                     LookupDeduplication) const override;
    std::variant<RecordWatch, StoreError> watch(ByteView key, std::function<void(StoreBatch)> cb,
                                                bool) const override;
};
std::optional<StoreError> Store::put(ByteView key, ByteView payload,
                                     std::function<void(bool)> completion) const {
    const Bytes k(key.begin(), key.end()), value(payload.begin(), payload.end());
    records[k].push_back(value);
    if (auto found = watches.find(k); found != watches.end() && found->second->active)
        found->second->callback(StoreBatch{{value}, {}, false});
    completion(true);
    return {};
}
std::optional<StoreError> Store::lookup(ByteView key, std::function<void(LookupResult)> completion,
                                        LookupDeduplication) const {
    if (fail_lookup)
        return StoreError::NotRunning;
    const auto found = records.find(Bytes(key.begin(), key.end()));
    completion(
        {StoreBatch{found == records.end() ? std::vector<Bytes>{} : found->second, {}, false},
         true});
    return {};
}
std::variant<RecordWatch, StoreError> Store::watch(ByteView key, std::function<void(StoreBatch)> cb,
                                                   bool) const {
    auto state = std::make_shared<WatchState>();
    state->callback = std::move(cb);
    state->ready.set_value(1);
    watches[Bytes(key.begin(), key.end())] = state;
    return RecordWatch(std::make_unique<Watch>(std::move(state)));
}
struct Harness {
    std::shared_ptr<Store> backend = std::make_shared<Store>();
    RecordStore store{backend};
    std::shared_ptr<Bytes> capability = std::make_shared<Bytes>(32, std::byte{8});
    std::shared_ptr<Identity> identity = std::make_shared<Identity>(
        std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1}))));
    std::deque<std::function<void()>> tasks, timers;
    int seen = 0, published = 0, caught_up = 0, failed = 0;
    DiscoveryCycleConfig config();
    DiscoveryCycleCallbacks callbacks();
    void drain();
    bool run(bool reject_lookup);
};
DiscoveryCycleConfig Harness::config() {
    AdvertisementLeaseConfig lease{{RecipientPublicKey{std::byte{2}}, InstanceId{std::byte{3}},
                                    SignalingInboxKey{std::byte{4}}, 0, 0, 0, 0},
                                   20,
                                   10,
                                   60,
                                   5,
                                   4,
                                   {2048, 1, 1024}};
    return {store,
            store,
            identity,
            capability,
            DiscoveryWatchLimits{{4096, 16, 1024}, 4, 60, 5},
            lease,
            MemoryBudget(8192),
            10ms,
            2,
            [] { return 100; }};
}
DiscoveryCycleCallbacks Harness::callbacks() {
    return {{[this](Advertisement, AdvertisementEvent) { ++seen; }, [](Advertisement) {},
             [this](DiscoveryWatchError, std::optional<StoreError>) { ++failed; },
             [this](bool ok) { caught_up += ok ? 1 : -1; }},
            {[this](std::uint64_t) { ++published; }, [this](AdvertisementLeaseError) { ++failed; }},
            [this](DiscoveryCycleError) { ++failed; }};
}
void Harness::drain() {
    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
}
bool Harness::run(bool reject_lookup) {
    backend->fail_lookup = reject_lookup;
    Executor executor = [this](auto task) { tasks.push_back(std::move(task)); };
    Scheduler scheduler = [this](auto, auto callback) -> CancelTimer {
        timers.push_back(std::move(callback));
        return [] {};
    };
    auto result = NativeDiscoveryCycle::create(config(), executor, scheduler, callbacks());
    auto *created = std::get_if<std::unique_ptr<NativeDiscoveryCycle>>(&result);
    if (!created)
        return false;
    auto cycle = std::move(*created);
    bool started = cycle->start();
    drain();
    if (reject_lookup)
        return !started && failed > 0 && !cycle->is_open();
    if (!started || seen != 1 || published != 1 || caught_up != 1 || failed != 0)
        return false;
    cycle->close();
    return !cycle->is_open() && !backend->watches.begin()->second->active;
}

bool rejects_invalid_public_options() {
    Scheduler scheduler = [](auto, auto) -> CancelTimer { return [] {}; };
    DiscoveryHandlers watch{[](Advertisement, PeerEvent) {}, [](bool) {}, [](WatchError) {}};
    WatchOptions options;
    options.max_peers = 0;
    bool watch_rejected = false;
    try {
        validate_watch(scheduler, options, watch);
    } catch (const std::invalid_argument &) {
        watch_rejected = true;
    }
    try {
        validate_announcement(scheduler, {}, {});
        return false;
    } catch (const std::invalid_argument &) {
        return watch_rejected;
    }
}
} // namespace

bool portable_discovery_smoke() {
    Harness happy, failed;
    return happy.run(false) && failed.run(true) && rejects_invalid_public_options();
}
