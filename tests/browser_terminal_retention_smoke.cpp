#include "../src/discovery/terminal_signal.hpp"

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
class DelayedStore final : public RecordStoreBackend {
  public:
    mutable std::function<void(bool)> completion;
    std::optional<StoreError> put(ByteView, ByteView,
                                  std::function<void(bool)> done) const override {
        completion = std::move(done);
        return std::nullopt;
    }
    std::optional<StoreError> lookup(ByteView, std::function<void(LookupResult)>,
                                     LookupDeduplication) const override {
        return StoreError::InvalidKey;
    }
    std::variant<RecordWatch, StoreError> watch(ByteView, std::function<void(StoreBatch)>,
                                                bool) const override {
        return StoreError::InvalidKey;
    }
};

bool retains_until_completion(bool success) {
    auto identity = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    auto box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{2})));
    auto backend = std::make_shared<DelayedStore>();
    std::weak_ptr<DelayedStore> weak = backend;
    SignalingFields fields{SignalingKind::Cancel,
                           {std::byte{1}},
                           {identity.public_key(), {std::byte{2}}},
                           {std::byte{3}},
                           63,
                           100,
                           150,
                           {}};
    {
        RecordStore store(backend);
        if (!publish_terminal_signal(store, Bytes(20, std::byte{1}), identity, fields,
                                     box.public_key(), Bytes(32, std::byte{4}), {100, 60, 0, 1024}))
            return false;
    }
    backend.reset();
    auto held = weak.lock();
    if (!held || !held->completion)
        return false;
    auto done = std::move(held->completion);
    done(success);
    done = {};
    held.reset();
    return weak.expired();
}
} // namespace

bool browser_terminal_retention_smoke() {
    return retains_until_completion(true) && retains_until_completion(false);
}

bool browser_terminal_budget_smoke() {
    auto identity = std::get<Identity>(Identity::import_seed(Bytes(32, std::byte{1})));
    auto box =
        std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, std::byte{2})));
    auto backend = std::make_shared<DelayedStore>();
    RecordStore store(backend);
    SignalingFields fields{SignalingKind::Cancel,
                           {std::byte{1}},
                           {identity.public_key(), {std::byte{2}}},
                           {std::byte{3}},
                           63,
                           100,
                           150,
                           {}};
    unsigned accepted = 0;
    for (unsigned index = 0; index < 129; ++index) {
        if (!publish_terminal_signal(store, Bytes(20, std::byte{1}), identity, fields,
                                     box.public_key(), Bytes(32, std::byte{4}), {100, 60, 0, 1024}))
            return accepted > 0 && accepted <= 128;
        ++accepted;
        auto done = std::move(backend->completion);
        done(true);
    }
    return false;
}
