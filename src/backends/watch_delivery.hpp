#pragma once

#include "../discovery/record_store.hpp"

#include <atomic>
#include <functional>
#include <future>
#include <mutex>

namespace p2pdc::detail {

// Delivers one watch's batches to its consumer, one at a time, with the record store's
// cancellation rules (record_store.hpp, native_store.hpp): cancel() waits for a delivery in
// flight unless called from it, and suppresses later ones; abandon(), for destruction, only
// suppresses them. Readiness is reported once, through the registration future: nonzero
// when ready, zero when the watch failed or ended first.
class WatchDelivery {
  public:
    explicit WatchDelivery(std::function<void(StoreBatch)> callback);
    void deliver(StoreBatch batch);
    void report(bool ready);
    void cancel();
    void abandon() noexcept { active_.store(false); }
    [[nodiscard]] bool active() const noexcept { return active_.load(); }
    [[nodiscard]] std::shared_future<std::size_t> registration() const { return registration_; }

  private:
    std::recursive_mutex mutex_;
    std::atomic<bool> active_{true};
    std::function<void(StoreBatch)> callback_;
    std::promise<std::size_t> ready_;
    std::shared_future<std::size_t> registration_;
    bool reported_ = false;
};

} // namespace p2pdc::detail
