#pragma once

#include "../src/discovery/record_store.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <vector>

namespace p2pdc::test {

using namespace std::chrono_literals;

inline Bytes bytes(std::initializer_list<int> values) {
    Bytes result;
    for (const int value : values)
        result.push_back(static_cast<std::byte>(value));
    return result;
}

inline std::array<std::byte, 20> test_key(int seed) {
    std::array<std::byte, 20> key{};
    for (int index = 0; index < 20; ++index)
        key[index] = static_cast<std::byte>((index + seed) % 256 ? (index + seed) % 256 : 1);
    return key;
}

// Waits for a put; nullopt if it did not complete in time.
inline std::optional<bool> put(const detail::RecordStore &store, ByteView key, const Bytes &value,
                               std::chrono::milliseconds wait = 10s) {
    auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    if (store.put(key, value, [done](bool ok) { done->set_value(ok); }))
        return std::nullopt;
    if (future.wait_for(wait) != std::future_status::ready)
        return std::nullopt;
    return future.get();
}

inline std::optional<detail::LookupResult>
lookup(const detail::RecordStore &store, ByteView key,
       detail::LookupDeduplication mode = detail::LookupDeduplication::IdAndContent,
       std::chrono::milliseconds wait = 15s) {
    auto done = std::make_shared<std::promise<detail::LookupResult>>();
    auto future = done->get_future();
    if (store.lookup(
            key, [done](detail::LookupResult result) { done->set_value(std::move(result)); }, mode))
        return std::nullopt;
    if (future.wait_for(wait) != std::future_status::ready)
        return std::nullopt;
    return future.get();
}

inline std::vector<Bytes> sorted(std::vector<Bytes> values) {
    std::ranges::sort(values);
    return values;
}

// Collects what a watch delivers, from any thread.
class Heard {
  public:
    auto callback() {
        return [this](detail::StoreBatch batch) {
            std::lock_guard lock(mutex_);
            for (auto &value : batch.values)
                values_.push_back(std::move(value));
            if (batch.error)
                errors_.push_back(*batch.error);
            changed_.notify_all();
        };
    }
    bool wait_values(std::size_t count, std::chrono::milliseconds wait = 10s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, wait, [&] { return values_.size() >= count; });
    }
    bool wait_error(std::chrono::milliseconds wait = 10s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, wait, [&] { return !errors_.empty(); });
    }
    std::vector<Bytes> values() {
        std::lock_guard lock(mutex_);
        return values_;
    }
    std::vector<detail::StoreError> errors() {
        std::lock_guard lock(mutex_);
        return errors_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Bytes> values_;
    std::vector<detail::StoreError> errors_;
};

// The registration token, or nullopt while not yet reported after wait.
inline std::optional<std::size_t> registered(const detail::RecordWatch &watch,
                                             std::chrono::milliseconds wait = 10s) {
    const auto future = watch.registration();
    if (future.wait_for(wait) != std::future_status::ready)
        return std::nullopt;
    return future.get();
}

} // namespace p2pdc::test
