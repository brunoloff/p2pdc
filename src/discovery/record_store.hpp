#pragma once

#include "p2pdc/frame.h"

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace p2pdc::detail {
enum class StoreError {
    InvalidKey,
    InvalidCallback,
    NotRunning,
    ResourceLimit,
    InvalidRecord,
    AllocationFailed,
    PermanentFailure
};
struct StoreLimits {
    std::size_t max_value_bytes;
    std::size_t max_values;
    std::size_t max_total_bytes;
};
struct StoreBatch {
    std::vector<Bytes> values;
    std::optional<StoreError> error;
    bool expired = false;
};
struct LookupResult {
    StoreBatch batch;
    // Completion does not establish network health or completeness.
    bool backend_success = false;
};
enum class LookupDeduplication { IdAndContent, Content };

class RecordWatchBackend {
  public:
    virtual ~RecordWatchBackend() = default;
    virtual void cancel() = 0;
    // A valid future reports a nonzero registration token or zero on failure.
    // Readiness does not establish network health or completeness.
    [[nodiscard]] virtual std::shared_future<std::size_t> registration() const = 0;
};

class RecordWatch {
  public:
    explicit RecordWatch(std::unique_ptr<RecordWatchBackend> backend);
    RecordWatch(RecordWatch &&) noexcept = default;
    RecordWatch &operator=(RecordWatch &&) = delete;
    ~RecordWatch() = default;
    void cancel();
    [[nodiscard]] std::shared_future<std::size_t> registration() const;

  private:
    std::unique_ptr<RecordWatchBackend> backend_;
};

class RecordStoreBackend {
  public:
    virtual ~RecordStoreBackend() = default;
    [[nodiscard]] virtual std::optional<StoreError>
    put(ByteView key, ByteView payload, std::function<void(bool)> completion) const = 0;
    [[nodiscard]] virtual std::optional<StoreError>
    lookup(ByteView key, std::function<void(LookupResult)> completion,
           LookupDeduplication deduplication) const = 0;
    [[nodiscard]] virtual std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const = 0;
};

// Backends own their asynchronous work. Destroying a watch suppresses future
// callbacks; already admitted callbacks may still complete. Backend callbacks
// may run inline or on another thread. All input spans must be copied before an
// asynchronous backend returns; failures are reported with StoreError.
class RecordStore {
  public:
    explicit RecordStore(std::shared_ptr<const RecordStoreBackend> backend);
    [[nodiscard]] std::optional<StoreError> put(ByteView key, ByteView payload,
                                                std::function<void(bool)> completion) const;
    [[nodiscard]] std::optional<StoreError> lookup(ByteView key,
                                                   std::function<void(LookupResult)> completion,
                                                   LookupDeduplication deduplication) const;
    [[nodiscard]] std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const;

  private:
    std::shared_ptr<const RecordStoreBackend> backend_;
};
} // namespace p2pdc::detail
