#pragma once

#include "../core/timer_producer.hpp"
#include "native_advertisement_lease.hpp"
#include "native_discovery_watch.hpp"

namespace p2pdc::detail {

enum class DiscoveryCycleError { Watch, Registration, Catchup, Lease, Scheduler };

struct DiscoveryCycleConfig {
    RecordStore watch_store;
    RecordStore lease_store;
    std::shared_ptr<const Identity> identity;
    std::shared_ptr<const Bytes> capability;
    std::optional<DiscoveryWatchLimits> watch_limits;
    std::optional<AdvertisementLeaseConfig> lease_config;
    MemoryBudget budget;
    std::chrono::milliseconds period;
    std::size_t retry_after_pulses;
    std::function<std::uint64_t()> now;
};

struct DiscoveryCycleCallbacks {
    DiscoveryWatchCallbacks watch;
    AdvertisementLeaseCallbacks lease;
    std::function<void(DiscoveryCycleError)> on_failure;
};

// Internal single-group owner for a watch, a lease, or both. Methods and all callbacks use one
// serialized executor and callbacks must not reenter or destroy the cycle. The scheduler, executor,
// clock captures and backends outlive outstanding backend work. start performs an initial pulse,
// then schedules subsequent pulses. The watcher subscribes during create, so
// watch callbacks may arrive before start. Closing either child stops only that child; close stops
// the timer before both children. Publication limits remain terminal: this does not replace
// exhausted leases. A failed catch-up closes the cycle and reports incomplete discovery; it does
// not mean the underlying listener itself failed. Retryable publication errors are retried no
// faster than retry_after_pulses scheduled pulses. A timer submission allocation failure cannot
// deliver a callback; the owner must poll is_open() or enforce a deadline to observe that
// exceptional case. is_open()==false does not close submission gates; call close() and drain
// admitted work before releasing the executor.
class NativeDiscoveryCycle {
  public:
    static std::variant<std::unique_ptr<NativeDiscoveryCycle>, StoreError, CryptoError>
    create(DiscoveryCycleConfig config, Executor executor, Scheduler scheduler,
           DiscoveryCycleCallbacks callbacks);
    ~NativeDiscoveryCycle();
    NativeDiscoveryCycle(const NativeDiscoveryCycle &) = delete;
    NativeDiscoveryCycle &operator=(const NativeDiscoveryCycle &) = delete;
    [[nodiscard]] bool start();
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] bool watch_available() const;
    [[nodiscard]] bool watch_recovering() const;
    [[nodiscard]] bool is_current(const Advertisement &value);
    [[nodiscard]] std::optional<std::vector<Advertisement>> current_peers();
    [[nodiscard]] std::optional<AdvertisementFields> published_fields();
    void stop_watching();
    void stop_announcing();
    void close();

  private:
    struct State;
    explicit NativeDiscoveryCycle(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
} // namespace p2pdc::detail
