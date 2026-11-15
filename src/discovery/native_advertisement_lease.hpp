#pragma once

#include "native_outbox.hpp"
#include "p2pdc/advertisement.h"
#include "p2pdc/discovery_key.h"

namespace p2pdc::detail {

enum class AdvertisementLeaseFailure {
    ClockRegressed,
    TimeOverflow,
    SequenceExhausted,
    EncodingFailed,
    AdmissionFailed,
    ExpiredPending,
    PublisherUnavailable,
    PublicationLimit
};
using AdvertisementLeaseError = std::variant<AdvertisementLeaseFailure, OutboxError>;
enum class AdvertisementLeaseTick { Submitted, NotDue, Pending, Failed, Closed };
struct AdvertisementLeaseConfig {
    AdvertisementFields fields;
    std::uint64_t lifetime;
    std::uint64_t refresh_interval;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
    std::size_t max_publications;
    OutboxLimits outbox;
    bool windowed = false;
};
struct AdvertisementLeaseCallbacks {
    std::function<void(std::uint64_t)> on_published;
    std::function<void(AdvertisementLeaseError)> on_error;
};

// Internal lease. It retains the identity and capability; mutable aliases must
// not change them. The backend, clock and executor outlive outstanding backend work.
// Methods and nonthrowing
// callbacks run on one serialized executor; callbacks must not reenter or
// destroy the lease. tick is caller-scheduled and uses nondecreasing UTC seconds.
// The configured sequence is used first; configured issue and expiry fields
// are replaced by the current time and lifetime for every fresh record.
// Retry republishes the exact pending bytes; if they expire, the lease fails and
// the owner must replace it with a fresh instance. Published means backend put
// completion, not peer observation. close cannot retract existing DHT values.
// OpenDHT gives each put a new value ID and may retain expired records. A hard
// publication count includes retries. In experimental windowed mode the limit
// applies per key window; watchers must separately support the same windows.
// Outbox errors can be reported inline from tick or retry and are retryable;
// AdvertisementLeaseFailure values are terminal and require closing the lease.
class NativeAdvertisementLease {
  public:
    static std::variant<std::unique_ptr<NativeAdvertisementLease>, CryptoError>
    create(RecordStore store, std::shared_ptr<const Identity> identity,
           std::shared_ptr<const Bytes> capability, AdvertisementLeaseConfig config,
           MemoryBudget budget, Executor executor, std::function<std::uint64_t()> now,
           AdvertisementLeaseCallbacks callbacks);
    ~NativeAdvertisementLease();
    NativeAdvertisementLease(const NativeAdvertisementLease &) = delete;
    NativeAdvertisementLease &operator=(const NativeAdvertisementLease &) = delete;
    [[nodiscard]] AdvertisementLeaseTick tick();
    [[nodiscard]] bool retry();
    [[nodiscard]] bool is_open() const;
    // Last successfully published record, only while it is locally unexpired.
    [[nodiscard]] std::optional<AdvertisementFields> published_fields();
    void close();

  private:
    struct State;
    explicit NativeAdvertisementLease(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace p2pdc::detail
