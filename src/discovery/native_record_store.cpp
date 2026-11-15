#include "native_store.hpp"

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
} // namespace p2pdc::detail
