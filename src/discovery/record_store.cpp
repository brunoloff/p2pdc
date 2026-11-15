#include "record_store.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
RecordWatch::RecordWatch(std::unique_ptr<RecordWatchBackend> backend)
    : backend_(std::move(backend)) {
    if (!backend_ || !backend_->registration().valid())
        throw std::invalid_argument("record watch requires a backend and registration future");
}
void RecordWatch::cancel() {
    if (!backend_)
        throw std::logic_error("record watch is moved from");
    backend_->cancel();
}
std::shared_future<std::size_t> RecordWatch::registration() const {
    if (!backend_)
        throw std::logic_error("record watch is moved from");
    return backend_->registration();
}

RecordStore::RecordStore(std::shared_ptr<const RecordStoreBackend> backend)
    : backend_(std::move(backend)) {
    if (!backend_)
        throw std::invalid_argument("record store requires a backend");
}
std::optional<StoreError> RecordStore::put(ByteView key, ByteView payload,
                                           std::function<void(bool)> completion) const {
    if (!backend_)
        throw std::logic_error("record store is moved from");
    return backend_->put(key, payload, std::move(completion));
}
std::optional<StoreError> RecordStore::lookup(ByteView key,
                                              std::function<void(LookupResult)> completion,
                                              LookupDeduplication deduplication) const {
    if (!backend_)
        throw std::logic_error("record store is moved from");
    return backend_->lookup(key, std::move(completion), deduplication);
}
std::variant<RecordWatch, StoreError>
RecordStore::watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const {
    if (!backend_)
        throw std::logic_error("record store is moved from");
    return backend_->watch(key, std::move(callback), deduplicate);
}
} // namespace p2pdc::detail
