#include "terminal_signal.hpp"

#include <iostream>
#include <stdexcept>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <memory>
#endif

namespace p2pdc::detail {
#ifdef __EMSCRIPTEN__
namespace {
std::size_t retained_publications = 0;
struct Retention {
    explicit Retention(const RecordStore &store) : store(store) { ++retained_publications; }
    ~Retention() {
        release();
        --retained_publications;
    }
    void release() {
        if (store) {
            store.reset();
        }
    }
    std::optional<RecordStore> store;
};
std::shared_ptr<Retention> retain_until_deadline(const RecordStore &store) {
    if (retained_publications >= 128)
        throw std::runtime_error("too many pending terminal signaling publications");
    auto retained = std::make_shared<Retention>(store);
    auto timer = std::make_unique<std::shared_ptr<Retention>>(retained);
    emscripten_async_call(
        [](void *arg) {
            std::unique_ptr<std::shared_ptr<Retention>> owner(
                static_cast<std::shared_ptr<Retention> *>(arg));
            (*owner)->release();
        },
        timer.release(), 15000);
    return retained;
}
} // namespace
#endif
bool publish_terminal_signal(const RecordStore &store, ByteView mailbox, const Identity &identity,
                             SignalingFields fields, const RecipientBoxPublicKey &recipient,
                             ByteView capability, SignalingPolicy policy) noexcept {
    try {
        if ((fields.kind != SignalingKind::Cancel && fields.kind != SignalingKind::Reject) ||
            !fields.payload.empty())
            throw std::invalid_argument("invalid terminal signaling record");
        auto encoded = encode_signaling(identity, fields, recipient, capability, policy);
        auto *record = std::get_if<Bytes>(&encoded);
        if (!record)
            throw std::runtime_error("could not encode terminal signaling record");
#ifdef __EMSCRIPTEN__
        auto retained = retain_until_deadline(store);
        auto completion = [retained](bool success) {
            retained->release();
            if (!success)
                std::cerr << "p2pdc: terminal signaling publication failed\n";
        };
#else
        auto completion = [](bool success) {
            if (!success)
                std::cerr << "p2pdc: terminal signaling publication failed\n";
        };
#endif
        auto error = store.put(mailbox, *record, std::move(completion));
        if (error) {
#ifdef __EMSCRIPTEN__
            retained->release();
#endif
            throw std::runtime_error("could not submit terminal signaling record");
        }
        return true;
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: " << error.what() << '\n';
        return false;
    }
}
} // namespace p2pdc::detail
