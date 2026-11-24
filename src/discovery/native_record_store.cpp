#include "native_store.hpp"

#ifdef P2PDC_HAVE_NETWORK_BACKENDS
#include "../backends/network_backends.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
class NativeRecordWatch final : public RecordWatchBackend {
  public:
    explicit NativeRecordWatch(NativeWatch watch) : watch_(std::move(watch)) {}
    void cancel() override { watch_.cancel(); }
    std::shared_future<std::size_t> registration() const override { return watch_.registration(); }

  private:
    NativeWatch watch_;
};

class NativeRecordStore final : public RecordStoreBackend {
  public:
    explicit NativeRecordStore(NativeStore store) : store_(std::move(store)) {}
    std::optional<StoreError> put(ByteView key, ByteView payload,
                                  std::function<void(bool)> completion) const override {
        return store_.put(key, payload, std::move(completion));
    }
    std::optional<StoreError> lookup(ByteView key, std::function<void(LookupResult)> completion,
                                     LookupDeduplication deduplication) const override {
        return store_.lookup(key, std::move(completion), deduplication);
    }
    std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const override {
        auto result = store_.watch(key, std::move(callback), deduplicate);
        if (auto *error = std::get_if<StoreError>(&result))
            return *error;
        return RecordWatch(
            std::make_unique<NativeRecordWatch>(std::get<NativeWatch>(std::move(result))));
    }

  private:
    NativeStore store_;
};
} // namespace

NativeStore::operator RecordStore() const {
    return RecordStore(std::make_shared<NativeRecordStore>(*this));
}

RecordStore make_native_record_store(std::shared_ptr<dht::DhtRunner> runner,
                                     const std::shared_ptr<NetworkBackends> &backends,
                                     StoreLimits limits) {
    RecordStore dht = NativeStore(std::move(runner), limits);
#ifdef P2PDC_HAVE_NETWORK_BACKENDS
    if (backends)
        return backends->store(std::move(dht), limits);
#else
    if (backends)
        throw std::logic_error("network backends in a build without them");
#endif
    return dht;
}
} // namespace p2pdc::detail
