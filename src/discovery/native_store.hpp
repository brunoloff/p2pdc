#pragma once

#include "record_store.hpp"

#include <opendht.h>

#include <functional>
#include <future>
#include <memory>
#include <optional>

namespace p2pdc::detail {

// Accumulates distinct lookup responses; watches deliberately do not use it.
class LookupAccumulator {
  public:
    LookupAccumulator(StoreLimits limits, std::function<void()> before_record_copy,
                      LookupDeduplication deduplication = LookupDeduplication::IdAndContent);
    bool receive(const std::vector<std::shared_ptr<dht::Value>> &values);
    StoreBatch batch;

  private:
    StoreLimits limits_;
    std::function<void()> before_record_copy_;
    std::vector<dht::Value::Id> ids_;
    std::size_t bytes_ = 0;
    std::size_t inspected_ = 0;
    LookupDeduplication deduplication_;
};

struct StoreWatchState;

// Callbacks run on OpenDHT threads, possibly inline if the runner stops during
// submission. They must not throw, block on DHT work, or destroy/join the runner.
// Explicit cancel waits for an in-flight callback, except self-cancel from that
// callback. Destruction does not wait: a delivery already admitted may run or
// finish; its captures must remain valid until that delivery has completed.
// Later deliveries are suppressed by an atomic gate checked under a mutex.
class NativeWatch {
  public:
    NativeWatch(const NativeWatch &) = delete;
    NativeWatch &operator=(const NativeWatch &) = delete;
    NativeWatch(NativeWatch &&) noexcept;
    NativeWatch &operator=(NativeWatch &&) = delete;
    ~NativeWatch();
    void cancel();
    // OpenDHT token 0 means registration failed; readiness is not network health.
    [[nodiscard]] std::shared_future<std::size_t> registration() const;

  private:
    friend class NativeStore;
    NativeWatch(std::weak_ptr<dht::DhtRunner> runner, dht::InfoHash key,
                std::shared_ptr<StoreWatchState> state);
    void request_cancel();
    std::weak_ptr<dht::DhtRunner> runner_;
    dht::InfoHash key_;
    std::shared_ptr<StoreWatchState> state_;
    std::shared_future<std::size_t> token_;
};

// Opaque, unauthenticated records; no completeness or ordering,
// delivery deadline or durable availability guarantee. Each put creates a fresh
// backend-assigned random value ID; no replacement or permanent refresh is used.
// Key input is exactly 20 nonzero-in-aggregate raw bytes, not a room-name hash.
// Lookup suppresses repeated responses with the same backend ID and bytes, but
// preserves distinct IDs and conflicting payloads. Watches do not deduplicate
// by default. Discovery may opt into deduplication by bytes regardless of ID;
// that mode inspects at most eight times max_values per lookup or watch batch,
// including discarded duplicates, and drops records beyond that.
// Limits count accepted payload bytes/records, excluding backend allocations and
// record overhead. Lookup limits are cumulative; watch limits apply per batch.
// Records over a limit are dropped and counted in StoreBatch::dropped; the
// lookup or watch continues, so hostile publications cannot end it.
// Caller keeps its runner reference alive and running until outstanding callbacks
// finish and watches are canceled/destroyed, then owns shutdown/join. Adapter
// destruction neither stops work nor joins. Submission/allocation errors on the
// caller thread throw. Allocation failures while copying callback records return
// AllocationFailed with the bounded prefix and terminate the lookup/watch.
// Optional before_record_copy is an internal allocation-fault test seam, invoked
// on the callback thread before each record copy; it may throw only bad_alloc or
// length_error. Ordinary consumers leave it empty.
class NativeStore {
  public:
    NativeStore(std::shared_ptr<dht::DhtRunner> runner, StoreLimits limits,
                std::function<void()> before_record_copy = {});
    operator RecordStore() const;
    [[nodiscard]] std::optional<StoreError> put(ByteView key, ByteView payload,
                                                std::function<void(bool)> completion) const;
    [[nodiscard]] std::optional<StoreError>
    lookup(ByteView key, std::function<void(LookupResult)> completion,
           LookupDeduplication deduplication = LookupDeduplication::IdAndContent) const;
    [[nodiscard]] std::variant<NativeWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback,
          bool deduplicate_by_content = false) const;

  private:
    [[nodiscard]] std::optional<StoreError> validate_key(ByteView key) const;
    std::shared_ptr<dht::DhtRunner> runner_;
    StoreLimits limits_;
    std::function<void()> before_record_copy_;
};

class NetworkBackends;
// The node's record store: the DHT, used all at once with the node's network backends when
// it has any (null backends: the DHT alone).
[[nodiscard]] RecordStore make_native_record_store(std::shared_ptr<dht::DhtRunner> runner,
                                                   const std::shared_ptr<NetworkBackends> &backends,
                                                   StoreLimits limits);

} // namespace p2pdc::detail
