#include "native_store.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
using BackendValues = std::vector<std::shared_ptr<dht::Value>>;

dht::InfoHash hash(ByteView key) {
    return {reinterpret_cast<const std::uint8_t *>(key.data()), key.size()};
}

bool duplicate(const StoreBatch &batch, const std::vector<dht::Value::Id> &ids,
               const dht::Value &value, LookupDeduplication deduplication) {
    if (deduplication == LookupDeduplication::IdAndContent && value.id == dht::Value::INVALID_ID) {
        return false;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto &stored = batch.values[i];
        if ((deduplication == LookupDeduplication::Content || ids[i] == value.id) &&
            stored.size() == value.data.size() &&
            std::equal(stored.begin(), stored.end(), value.data.begin(),
                       [](std::byte a, std::uint8_t b) {
                           return std::to_integer<std::uint8_t>(a) == b;
                       })) {
            return true;
        }
    }
    return false;
}

struct AppendContext {
    StoreBatch &batch;
    std::size_t &bytes;
    StoreLimits limits;
    const std::function<void()> &before_record_copy;
    std::vector<dht::Value::Id> *ids;
    LookupDeduplication deduplication;
    std::size_t *inspected;
};

bool append_one(AppendContext &context, const std::shared_ptr<dht::Value> &value) {
    if (context.inspected && (*context.inspected == std::numeric_limits<std::size_t>::max() ||
                              *context.inspected / 8 >= context.limits.max_values)) {
        context.batch.error = StoreError::ResourceLimit;
        return false;
    }
    if (context.inspected)
        ++*context.inspected;
    if (!value) {
        context.batch.error = StoreError::InvalidRecord;
        return false;
    }
    if (context.ids && duplicate(context.batch, *context.ids, *value, context.deduplication))
        return true;
    const auto size = value->data.size();
    if (size > context.limits.max_value_bytes ||
        context.batch.values.size() >= context.limits.max_values ||
        size > context.limits.max_total_bytes - context.bytes) {
        context.batch.error = StoreError::ResourceLimit;
        return false;
    }
    if (context.before_record_copy)
        context.before_record_copy();
    Bytes payload(size);
    std::transform(value->data.begin(), value->data.end(), payload.begin(),
                   [](std::uint8_t byte) { return static_cast<std::byte>(byte); });
    context.batch.values.push_back(std::move(payload));
    if (context.ids)
        context.ids->push_back(value->id);
    context.bytes += size;
    return true;
}

bool append(StoreBatch &batch, std::size_t &bytes, const BackendValues &values, StoreLimits limits,
            const std::function<void()> &before_record_copy,
            std::vector<dht::Value::Id> *ids = nullptr,
            LookupDeduplication deduplication = LookupDeduplication::IdAndContent,
            std::size_t *inspected = nullptr) {
    AppendContext context{batch, bytes, limits, before_record_copy, ids, deduplication, inspected};
    try {
        for (const auto &value : values)
            if (!append_one(context, value))
                return false;
    } catch (const std::bad_alloc &) {
        batch.error = StoreError::AllocationFailed;
        return false;
    } catch (const std::length_error &) {
        batch.error = StoreError::AllocationFailed;
        return false;
    }
    return true;
}

struct LookupState {
    std::mutex mutex;
    LookupAccumulator accumulator;
    LookupResult result;
    std::function<void(LookupResult)> completion;
    bool finished = false;

    LookupState(StoreLimits limits, std::function<void()> before_record_copy,
                LookupDeduplication deduplication)
        : accumulator(limits, std::move(before_record_copy), deduplication) {}

    bool receive(const BackendValues &values) {
        std::lock_guard lock(mutex);
        return !finished && accumulator.receive(values);
    }

    void finish(bool success) {
        std::unique_lock lock(mutex);
        if (finished) {
            return;
        }
        finished = true;
        result.backend_success = success;
        result.batch = std::move(accumulator.batch);
        auto delivered = std::move(result);
        auto callback = std::move(completion);
        lock.unlock();
        callback(std::move(delivered));
    }
};
} // namespace

LookupAccumulator::LookupAccumulator(StoreLimits limits, std::function<void()> before_record_copy,
                                     LookupDeduplication deduplication)
    : limits_(limits), before_record_copy_(std::move(before_record_copy)),
      deduplication_(deduplication) {
    if (deduplication != LookupDeduplication::IdAndContent &&
        deduplication != LookupDeduplication::Content)
        throw std::invalid_argument("invalid lookup deduplication policy");
}

bool LookupAccumulator::receive(const BackendValues &values) {
    return !batch.error &&
           append(batch, bytes_, values, limits_, before_record_copy_, &ids_, deduplication_,
                  deduplication_ == LookupDeduplication::Content ? &inspected_ : nullptr);
}

struct StoreWatchState {
    std::recursive_mutex gate;
    std::atomic_bool active{true};
    StoreLimits limits;
    bool deduplicate_by_content = false;
    std::function<void()> before_record_copy;
    std::function<void(StoreBatch)> callback;

    bool receive(const BackendValues &values, bool expired) {
        std::lock_guard lock(gate);
        if (!active.load()) {
            return false;
        }
        StoreBatch batch;
        batch.expired = expired;
        std::size_t bytes = 0;
        std::size_t inspected = 0;
        std::vector<dht::Value::Id> ids;
        const bool keep = deduplicate_by_content
                              ? append(batch, bytes, values, limits, before_record_copy, &ids,
                                       LookupDeduplication::Content, &inspected)
                              : append(batch, bytes, values, limits, before_record_copy);
        callback(std::move(batch));
        if (!keep) {
            active.store(false);
        }
        return keep && active.load();
    }
};

NativeWatch::NativeWatch(std::weak_ptr<dht::DhtRunner> runner, dht::InfoHash key,
                         std::shared_ptr<StoreWatchState> state)
    : runner_(std::move(runner)), key_(key), state_(std::move(state)) {}

NativeWatch::NativeWatch(NativeWatch &&) noexcept = default;

NativeWatch::~NativeWatch() {
    try {
        request_cancel();
    } catch (const std::exception &error) {
        std::cerr << "NativeWatch: failed to schedule OpenDHT cancellation: " << error.what()
                  << '\n';
    }
}

void NativeWatch::request_cancel() {
    if (state_ && state_->active.exchange(false)) {
        if (auto runner = runner_.lock(); runner && token_.valid()) {
            runner->cancelListen(key_, token_);
        }
    }
}

void NativeWatch::cancel() {
    if (state_) {
        std::lock_guard lock(state_->gate);
        request_cancel();
    }
}

std::shared_future<std::size_t> NativeWatch::registration() const { return token_; }

NativeStore::NativeStore(std::shared_ptr<dht::DhtRunner> runner, StoreLimits limits,
                         std::function<void()> before_record_copy)
    : runner_(std::move(runner)), limits_(limits),
      before_record_copy_(std::move(before_record_copy)) {
    if (!runner_ || limits.max_values == 0 || limits.max_value_bytes > dht::MAX_VALUE_SIZE) {
        throw std::invalid_argument("native store requires a runner and valid record limits");
    }
}

std::optional<StoreError> NativeStore::validate_key(ByteView key) const {
    if (key.size() != 20 || std::all_of(key.begin(), key.end(),
                                        [](std::byte value) { return value == std::byte{0}; })) {
        return StoreError::InvalidKey;
    }
    if (!runner_->isRunning()) {
        return StoreError::NotRunning;
    }
    return std::nullopt;
}

// Byte spans keep malformed keys testable at this untrusted-input boundary.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<StoreError> NativeStore::put(ByteView key, ByteView payload,
                                           std::function<void(bool)> completion) const {
    if (!completion) {
        return StoreError::InvalidCallback;
    }
    if (const auto error = validate_key(key)) {
        return error;
    }
    if (payload.size() > limits_.max_value_bytes || payload.size() > limits_.max_total_bytes) {
        return StoreError::ResourceLimit;
    }
    dht::Blob bytes(payload.size());
    std::transform(payload.begin(), payload.end(), bytes.begin(),
                   [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
    runner_->put(hash(key), dht::Value(std::move(bytes)), std::move(completion));
    return std::nullopt;
}

std::optional<StoreError> NativeStore::lookup(ByteView key,
                                              std::function<void(LookupResult)> completion,
                                              LookupDeduplication deduplication) const {
    if (!completion) {
        return StoreError::InvalidCallback;
    }
    if (const auto error = validate_key(key)) {
        return error;
    }
    auto state = std::make_shared<LookupState>(limits_, before_record_copy_, deduplication);
    state->completion = std::move(completion);
    runner_->get(
        hash(key), [state](const BackendValues &values) { return state->receive(values); },
        dht::DoneCallbackSimple([state](bool success) { state->finish(success); }));
    return std::nullopt;
}

std::variant<NativeWatch, StoreError> NativeStore::watch(ByteView key,
                                                         std::function<void(StoreBatch)> callback,
                                                         bool deduplicate_by_content) const {
    if (!callback) {
        return StoreError::InvalidCallback;
    }
    if (const auto error = validate_key(key)) {
        return *error;
    }
    auto state = std::make_shared<StoreWatchState>();
    state->limits = limits_;
    state->deduplicate_by_content = deduplicate_by_content;
    state->before_record_copy = before_record_copy_;
    state->callback = std::move(callback);
    NativeWatch watch(runner_, hash(key), state);
    dht::ValueCallback receive = [state](const auto &values, bool expired) {
        return state->receive(values, expired);
    };
    watch.token_ = runner_->listen(hash(key), std::move(receive)).share();
    return watch;
}

} // namespace p2pdc::detail
