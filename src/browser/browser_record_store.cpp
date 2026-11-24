#include "browser_record_store.hpp"

#include "../discovery/discovery_backends_json.hpp"

#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

using namespace p2pdc;
using namespace p2pdc::detail;

EM_JS(unsigned, proxy_create,
      (const char *url, unsigned size, unsigned value_limit, unsigned count_limit,
       unsigned byte_limit, unsigned timeout),
      {
          try {
              const text =
                  new TextDecoder('utf-8', {fatal : true}).decode(HEAPU8.slice(url, url + size));
              return Module.p2pdcRecordBridge.create(text, {
                  maxValueBytes : value_limit,
                  maxValues : count_limit,
                  maxTotalBytes : byte_limit,
                  timeoutMs : timeout,
              });
          } catch (error) {
              console.error('p2pdc: invalid browser proxy configuration');
              return 0;
          }
      });
EM_JS(int, proxy_start,
      (unsigned store, unsigned operation, unsigned kind, const std::byte *key,
       const std::byte *value, unsigned size, int deduplicate),
      {
          try {
              const address = HEAPU8.slice(key, key + 20);
              if (kind == 0)
                  Module.p2pdcRecordBridge.put(store, operation, address,
                                               HEAPU8.slice(value, value + size));
              else if (kind == 1)
                  Module.p2pdcRecordBridge.lookup(store, operation, address, !!deduplicate);
              else
                  Module.p2pdcRecordBridge.watch(store, operation, address);
              return 1;
          } catch (error) {
              console.error('p2pdc: cannot start proxy operation');
              return 0;
          }
      });
EM_JS(void, proxy_cancel, (unsigned store, unsigned operation), {
    try {
        Module.p2pdcRecordBridge.cancel(store, operation);
    } catch (error) {
        console.error('p2pdc: cannot cancel proxy operation');
    }
});
EM_JS(void, proxy_close, (unsigned store), {
    try {
        Module.p2pdcRecordBridge.close(store);
    } catch (error) {
        console.error('p2pdc: cannot close proxy store');
    }
});

namespace p2pdc::detail {
namespace {
enum class Kind { Put, Lookup, Watch };
struct Operation {
    explicit Operation(Kind type) : kind(type), registered(ready.get_future().share()) {}
    Kind kind;
    std::function<void(bool)> put;
    std::function<void(LookupResult)> lookup;
    std::function<void(StoreBatch)> watch;
    LookupResult result;
    std::size_t bytes = 0;
    std::promise<std::size_t> ready;
    std::shared_future<std::size_t> registered;
    bool readiness_reported = false;
};

struct StoreState : std::enable_shared_from_this<StoreState> {
    StoreState(unsigned identifier, StoreLimits bounds) : id(identifier), limits(bounds) {}
    ~StoreState();
    unsigned id;
    StoreLimits limits;
    unsigned next = 0;
    std::map<unsigned, std::shared_ptr<Operation>> operations;
    std::optional<unsigned> add(std::shared_ptr<Operation> operation);
    void cancel(unsigned operation);
    bool event(unsigned operation, unsigned kind, ByteView value, unsigned detail);
    bool lookup_record(const std::shared_ptr<Operation> &item, ByteView value);
    bool watch_record(const std::shared_ptr<Operation> &item, ByteView value, bool expired);
    void fail(unsigned operation, StoreError error);
};

std::map<unsigned, std::weak_ptr<StoreState>> stores;

bool valid_key(ByteView key) {
    return key.size() == 20 && std::any_of(key.begin(), key.end(),
                                           [](std::byte value) { return value != std::byte{}; });
}
StoreError map_error(unsigned value) {
    switch (value) {
    case 3:
        return StoreError::ResourceLimit;
    case 4:
        return StoreError::InvalidRecord;
    case 5:
        return StoreError::AllocationFailed;
    case 6:
        return StoreError::PermanentFailure;
    default:
        return StoreError::NotRunning;
    }
}
void readiness(Operation &operation, bool success) {
    if (!operation.readiness_reported) {
        operation.readiness_reported = true;
        operation.ready.set_value(success ? 1 : 0);
    }
}
Bytes copy_value(ByteView value) {
    return value.empty() ? Bytes{} : Bytes(value.begin(), value.end());
}

StoreState::~StoreState() {
    stores.erase(id);
    proxy_close(id);
}
std::optional<unsigned> StoreState::add(std::shared_ptr<Operation> operation) {
    if (operations.size() >= 128 || next == std::numeric_limits<unsigned>::max())
        return std::nullopt;
    if (operation->kind == Kind::Lookup) {
        std::size_t active_lookups = 0;
        for (const auto &[unused, value] : operations) {
            (void)unused;
            if (value->kind == Kind::Lookup)
                ++active_lookups;
        }
        constexpr std::size_t lookup_budget = 16 * 1024 * 1024;
        if (active_lookups >= lookup_budget / limits.max_total_bytes)
            return std::nullopt;
    }
    const auto identifier = ++next;
    operations.emplace(identifier, std::move(operation));
    return identifier;
}
void StoreState::cancel(unsigned operation) {
    auto found = operations.find(operation);
    if (found == operations.end())
        return;
    auto value = std::move(found->second);
    operations.erase(found);
    proxy_cancel(id, operation);
    if (value->kind == Kind::Watch)
        readiness(*value, false);
}
void StoreState::fail(unsigned operation, StoreError error) {
    auto found = operations.find(operation);
    if (found == operations.end())
        return;
    auto value = std::move(found->second);
    operations.erase(found);
    if (value->kind == Kind::Put)
        value->put(false);
    else if (value->kind == Kind::Lookup) {
        value->result.batch.error = error;
        value->lookup(std::move(value->result));
    } else {
        value->watch(StoreBatch{{}, error, false, 0});
        readiness(*value, false);
    }
}
// Records are untrusted: over-limit records are dropped and counted, and the
// operation continues (see StoreBatch).
bool StoreState::lookup_record(const std::shared_ptr<Operation> &item, ByteView value) {
    if (value.size() > limits.max_value_bytes ||
        item->result.batch.values.size() >= limits.max_values ||
        value.size() > limits.max_total_bytes - item->bytes) {
        ++item->result.batch.dropped;
        return true;
    }
    item->bytes += value.size();
    item->result.batch.values.push_back(copy_value(value));
    return true;
}
bool StoreState::watch_record(const std::shared_ptr<Operation> &item, ByteView value,
                              bool expired) {
    if (value.size() > limits.max_value_bytes || value.size() > limits.max_total_bytes) {
        item->watch(StoreBatch{{}, {}, expired, 1});
        return true;
    }
    item->watch(StoreBatch{{copy_value(value)}, {}, expired, 0});
    return true;
}
bool StoreState::event(unsigned operation, unsigned kind, ByteView value, unsigned detail) {
    auto found = operations.find(operation);
    if (found == operations.end())
        return false;
    auto item = found->second;
    if (kind == 0 && item->kind == Kind::Put) {
        operations.erase(found);
        item->put(detail == 1);
    } else if (kind == 1 && item->kind == Kind::Lookup) {
        return lookup_record(item, value);
    } else if (kind == 2 && item->kind == Kind::Lookup) {
        operations.erase(found);
        if (detail)
            item->result.batch.error = map_error(detail);
        item->result.backend_success = detail == 0;
        item->lookup(std::move(item->result));
    } else if (kind == 3 && item->kind == Kind::Watch) {
        readiness(*item, detail == 1);
    } else if (kind == 4 && item->kind == Kind::Watch) {
        return watch_record(item, value, detail == 1);
    } else if (kind == 5 && item->kind == Kind::Watch) {
        fail(operation, map_error(detail));
    } else {
        fail(operation, StoreError::InvalidRecord);
        return false;
    }
    return true;
}

class BrowserWatch final : public RecordWatchBackend {
  public:
    BrowserWatch(std::shared_ptr<StoreState> owner, unsigned identifier,
                 std::shared_future<std::size_t> registration)
        : state_(std::move(owner)), operation_(identifier), registration_(std::move(registration)) {
    }
    ~BrowserWatch() override {
        try {
            cancel();
        } catch (const std::exception &) {
            std::cerr << "p2pdc: browser watch cleanup failed\n";
        }
    }
    void cancel() override { state_->cancel(operation_); }
    std::shared_future<std::size_t> registration() const override { return registration_; }

  private:
    std::shared_ptr<StoreState> state_;
    unsigned operation_;
    std::shared_future<std::size_t> registration_;
};

class BrowserStore final : public RecordStoreBackend {
  public:
    explicit BrowserStore(std::shared_ptr<StoreState> state) : state_(std::move(state)) {}
    std::optional<StoreError> put(ByteView key, ByteView payload,
                                  std::function<void(bool)> completion) const override;
    std::optional<StoreError> lookup(ByteView key, std::function<void(LookupResult)> completion,
                                     LookupDeduplication deduplication) const override;
    std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const override;

  private:
    std::optional<unsigned> start(ByteView key, ByteView value,
                                  std::shared_ptr<Operation> operation, bool deduplicate) const;
    std::shared_ptr<StoreState> state_;
};

std::optional<unsigned> BrowserStore::start(ByteView key, ByteView value,
                                            std::shared_ptr<Operation> operation,
                                            bool deduplicate) const {
    auto identifier = state_->add(operation);
    if (!identifier)
        return std::nullopt;
    if (!proxy_start(state_->id, *identifier, static_cast<unsigned>(operation->kind), key.data(),
                     value.data(), value.size(), deduplicate)) {
        state_->cancel(*identifier);
        return std::nullopt;
    }
    return identifier;
}
std::optional<StoreError> BrowserStore::put(ByteView key, ByteView payload,
                                            std::function<void(bool)> completion) const {
    if (!completion)
        return StoreError::InvalidCallback;
    if (!valid_key(key))
        return StoreError::InvalidKey;
    if (payload.size() > state_->limits.max_value_bytes ||
        payload.size() > state_->limits.max_total_bytes)
        return StoreError::ResourceLimit;
    auto operation = std::make_shared<Operation>(Kind::Put);
    operation->put = std::move(completion);
    return start(key, payload, std::move(operation), false)
               ? std::nullopt
               : std::optional{StoreError::ResourceLimit};
}
std::optional<StoreError> BrowserStore::lookup(ByteView key,
                                               std::function<void(LookupResult)> completion,
                                               LookupDeduplication deduplication) const {
    if (!completion)
        return StoreError::InvalidCallback;
    if (!valid_key(key))
        return StoreError::InvalidKey;
    if (deduplication != LookupDeduplication::IdAndContent &&
        deduplication != LookupDeduplication::Content)
        throw std::invalid_argument("invalid lookup deduplication policy");
    auto operation = std::make_shared<Operation>(Kind::Lookup);
    operation->lookup = std::move(completion);
    return start(key, {}, std::move(operation), deduplication == LookupDeduplication::Content)
               ? std::nullopt
               : std::optional{StoreError::ResourceLimit};
}
std::variant<RecordWatch, StoreError> BrowserStore::watch(ByteView key,
                                                          std::function<void(StoreBatch)> callback,
                                                          bool deduplicate) const {
    if (!callback)
        return StoreError::InvalidCallback;
    if (!valid_key(key))
        return StoreError::InvalidKey;
    auto operation = std::make_shared<Operation>(Kind::Watch);
    operation->watch = std::move(callback);
    auto identifier = start(key, {}, operation, deduplicate);
    if (!identifier)
        return StoreError::ResourceLimit;
    try {
        return RecordWatch(
            std::make_unique<BrowserWatch>(state_, *identifier, operation->registered));
    } catch (...) {
        state_->cancel(*identifier);
        throw;
    }
}

} // namespace

RecordStore make_browser_record_store(const std::vector<::p2pdc::DiscoveryBackend> &backends,
                                      StoreLimits limits, unsigned timeout_ms) {
    const std::string proxy_url = discovery_backends_json(backends);
    if (proxy_url.size() > 4096 || !limits.max_value_bytes || limits.max_value_bytes > 32768 ||
        !limits.max_values || limits.max_values > 1024 || !limits.max_total_bytes ||
        limits.max_total_bytes > 16 * 1024 * 1024 || !timeout_ms || timeout_ms > 600000)
        throw std::invalid_argument("invalid browser record store limits");
    const auto id = proxy_create(proxy_url.data(), proxy_url.size(), limits.max_value_bytes,
                                 limits.max_values, limits.max_total_bytes, timeout_ms);
    if (!id)
        throw std::invalid_argument("invalid browser proxy configuration");
    try {
        auto state = std::make_shared<StoreState>(id, limits);
        stores.emplace(id, state);
        return RecordStore(std::make_shared<BrowserStore>(std::move(state)));
    } catch (...) {
        proxy_close(id);
        throw;
    }
}
} // namespace p2pdc::detail

extern "C" {
EMSCRIPTEN_KEEPALIVE void *p2pdc_proxy_alloc(unsigned size) {
    return std::malloc(std::max(1U, size));
}
EMSCRIPTEN_KEEPALIVE void p2pdc_proxy_free(void *pointer) { std::free(pointer); }
EMSCRIPTEN_KEEPALIVE int p2pdc_proxy_event(unsigned store, unsigned operation, unsigned kind,
                                           const std::byte *data, unsigned size, unsigned detail) {
    const auto found = stores.find(store);
    if (found == stores.end())
        return 0;
    auto state = found->second.lock();
    if (!state || (!data && size))
        return 0;
    try {
        return state->event(operation, kind, ByteView(data, size), detail) ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: proxy event delivery failed: " << error.what() << '\n';
        try {
            state->fail(operation, StoreError::AllocationFailed);
        } catch (const std::exception &) {
            std::cerr << "p2pdc: proxy failure callback failed\n";
        }
        return 0;
    }
}
}
