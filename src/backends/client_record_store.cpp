#include "client_record_store.hpp"

#include "watch_delivery.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {

class ClientWatch final : public RecordWatchBackend {
  public:
    ClientWatch(std::shared_ptr<BackendLoop> loop, std::shared_ptr<RecordClient> client,
                std::shared_ptr<WatchDelivery> delivery,
                std::shared_ptr<std::optional<RecordClient::WatchId>> id)
        : loop_(std::move(loop)), client_(std::move(client)), delivery_(std::move(delivery)),
          id_(std::move(id)) {}
    ~ClientWatch() override {
        delivery_->abandon();
        stop();
    }
    void cancel() override {
        delivery_->cancel();
        stop();
    }
    std::shared_future<std::size_t> registration() const override {
        return delivery_->registration();
    }

  private:
    void stop() {
        loop_->post([client = client_, id = id_] {
            if (*id)
                client->unlisten(**id);
            id->reset();
        });
    }
    std::shared_ptr<BackendLoop> loop_;
    std::shared_ptr<RecordClient> client_;
    std::shared_ptr<WatchDelivery> delivery_;
    std::shared_ptr<std::optional<RecordClient::WatchId>> id_; // loop thread only
};

RecordClient::Listener listener_for(const std::shared_ptr<WatchDelivery> &delivery) {
    return {[delivery](BackendRecord record) {
                delivery->deliver(StoreBatch{{std::move(record.bytes)}, {}, false, 0});
            },
            [delivery](bool ready) { delivery->report(ready); },
            [delivery](std::optional<BackendFailure> failure) {
                delivery->report(false);
                if (failure)
                    delivery->deliver(StoreBatch{{}, store_error(*failure), false, 0});
            }};
}

class ClientStore final : public RecordStoreBackend {
  public:
    ClientStore(std::shared_ptr<BackendLoop> loop, std::shared_ptr<RecordClient> client,
                StoreLimits limits)
        : loop_(std::move(loop)), client_(std::move(client)), limits_(limits) {}

    std::optional<StoreError> put(ByteView key, ByteView payload,
                                  std::function<void(bool)> completion) const override {
        const auto address = record_key(key);
        if (!completion)
            return StoreError::InvalidCallback;
        if (!address)
            return StoreError::InvalidKey;
        if (payload.size() > limits_.max_value_bytes || payload.size() > limits_.max_total_bytes)
            return StoreError::ResourceLimit;
        const bool posted = loop_->post([client = client_, address = *address, limits = limits_,
                                         value = Bytes(payload.begin(), payload.end()),
                                         completion = std::move(completion)]() mutable {
            client->put(address, std::move(value), limits,
                        [completion](auto failure) { completion(!failure); });
        });
        return posted ? std::nullopt : std::optional(StoreError::NotRunning);
    }

    std::optional<StoreError> lookup(ByteView key, std::function<void(LookupResult)> completion,
                                     LookupDeduplication deduplication) const override {
        const auto address = record_key(key);
        if (!completion)
            return StoreError::InvalidCallback;
        if (!address)
            return StoreError::InvalidKey;
        const bool by_content = deduplication == LookupDeduplication::Content;
        const bool posted = loop_->post([client = client_, address = *address, limits = limits_,
                                         by_content, completion = std::move(completion)] {
            client->get(address, limits, by_content, [completion](auto answer) {
                LookupResult result;
                if (auto *failure = std::get_if<BackendFailure>(&answer)) {
                    result.batch.error = store_error(*failure);
                } else {
                    result.batch = std::get<StoreBatch>(std::move(answer));
                    result.backend_success = true;
                }
                completion(std::move(result));
            });
        });
        return posted ? std::nullopt : std::optional(StoreError::NotRunning);
    }

    std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool) const override {
        const auto address = record_key(key);
        if (!callback)
            return StoreError::InvalidCallback;
        if (!address)
            return StoreError::InvalidKey;
        auto delivery = std::make_shared<WatchDelivery>(std::move(callback));
        auto id = std::make_shared<std::optional<RecordClient::WatchId>>();
        const bool posted =
            loop_->post([client = client_, address = *address, limits = limits_, delivery, id] {
                if (delivery->active())
                    *id = client->listen(address, limits, listener_for(delivery));
            });
        if (!posted)
            return StoreError::NotRunning;
        return RecordWatch(std::make_unique<ClientWatch>(loop_, client_, delivery, id));
    }

  private:
    std::shared_ptr<BackendLoop> loop_;
    std::shared_ptr<RecordClient> client_;
    StoreLimits limits_;
};

} // namespace

RecordStore make_client_record_store(std::shared_ptr<BackendLoop> loop,
                                     std::shared_ptr<RecordClient> client, StoreLimits limits) {
    if (!loop || !client || !limits.max_value_bytes || !limits.max_values ||
        !limits.max_total_bytes)
        throw std::invalid_argument("invalid network record store");
    return RecordStore(std::make_shared<ClientStore>(std::move(loop), std::move(client), limits));
}

} // namespace p2pdc::detail
