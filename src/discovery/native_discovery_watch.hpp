#pragma once

#include "advertisement_tracker.hpp"
#include "native_mailbox.hpp"
#include "p2pdc/discovery_key.h"
#include "recovering_mailbox.hpp"

namespace p2pdc::detail {

enum class DiscoveryWatchError { Store, ResourceLimit, ClockRegressed, CryptoFailure };
struct DiscoveryWatchCallbacks {
    std::function<void(Advertisement, AdvertisementEvent)> on_peer;
    std::function<void(Advertisement)> on_expired;
    std::function<void(DiscoveryWatchError, std::optional<StoreError>)> on_error;
    std::function<void(bool)> on_catchup;
    std::function<void(RecoveryState)> on_status{};
};
struct DiscoveryWatchLimits {
    MailboxLimits mailbox;
    std::size_t max_entries;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
    bool windowed = false;
    std::optional<RecoveryOptions> recovery{};
};

// Internal discovery watcher. It retains its capability; mutable aliases must
// not change it. The backend, clock and executor outlive outstanding backend work.
// Methods and nonthrowing callbacks
// run on the same serialized executor; callbacks must not reenter or destroy the
// watcher. now returns nondecreasing UTC seconds. Records are decrypted and
// signature-checked before tracking; peer identity is not application consent.
// tick must be called periodically to report expiry and prune watermarks.
// In experimental windowed mode, tick also registers and catches up rotating
// keys automatically; registration() is invalid and catch_up() is unavailable.
// on_catchup runs once for each current window range after its lookups finish.
// A full tracker ignores new peers (logged once) and stays healthy. Local
// clock/crypto failure makes the watcher unhealthy; the owner closes it and
// reports incomplete discovery. DHT is best effort.
// The supplied budget covers mailbox payloads; max_entries separately bounds
// tracker entries and expiry-event allocations, not OpenDHT or allocator overhead.
class NativeDiscoveryWatch {
  public:
    static std::variant<std::unique_ptr<NativeDiscoveryWatch>, StoreError, CryptoError>
    create(RecordStore store, std::shared_ptr<const Bytes> capability, DiscoveryWatchLimits limits,
           MemoryBudget budget, Executor executor, std::function<std::uint64_t()> now,
           DiscoveryWatchCallbacks callbacks);
    ~NativeDiscoveryWatch();
    NativeDiscoveryWatch(const NativeDiscoveryWatch &) = delete;
    NativeDiscoveryWatch &operator=(const NativeDiscoveryWatch &) = delete;
    [[nodiscard]] std::shared_future<std::size_t> registration() const;
    [[nodiscard]] bool automatically_catches_up() const;
    [[nodiscard]] CatchupResult catch_up();
    [[nodiscard]] bool tick();
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] bool available() const;
    [[nodiscard]] bool recovering() const;
    [[nodiscard]] bool is_current(const Advertisement &value);
    [[nodiscard]] std::optional<std::vector<Advertisement>> current_peers();
    void close();

  private:
    struct State;
    explicit NativeDiscoveryWatch(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace p2pdc::detail
