#include "watch_delivery.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

WatchDelivery::WatchDelivery(std::function<void(StoreBatch)> callback)
    : callback_(std::move(callback)), registration_(ready_.get_future().share()) {
    if (!callback_)
        throw std::invalid_argument("watch delivery requires a callback");
}

void WatchDelivery::deliver(StoreBatch batch) {
    std::lock_guard lock(mutex_);
    if (active_.load())
        callback_(std::move(batch));
}

void WatchDelivery::report(bool ready) {
    std::lock_guard lock(mutex_);
    if (!std::exchange(reported_, true))
        ready_.set_value(ready && active_.load() ? 1 : 0);
}

void WatchDelivery::cancel() {
    std::lock_guard lock(mutex_);
    active_.store(false);
    if (!std::exchange(reported_, true))
        ready_.set_value(0);
}

} // namespace p2pdc::detail
