#pragma once

#include "p2pdc/frame.h"

#include <sodium.h>

#include <memory>

namespace p2pdc::detail {

inline void wipe_bytes(Bytes &bytes) noexcept {
    if (!bytes.empty())
        sodium_memzero(bytes.data(), bytes.size());
}

struct SecretBytesDeleter {
    void operator()(const Bytes *bytes) const noexcept {
        auto *owned = const_cast<Bytes *>(bytes);
        wipe_bytes(*owned);
        delete owned;
    }
};

// Shared, immutable copy of secret bytes (such as a capability) that is wiped
// with sodium_memzero when its last owner releases it. The storage is ordinary
// heap memory: it is not locked or guarded, and copies made from it are not
// wiped automatically.
[[nodiscard]] inline std::shared_ptr<const Bytes> make_secret_bytes(ByteView value) {
    return {new Bytes(value.begin(), value.end()), SecretBytesDeleter{}};
}

} // namespace p2pdc::detail
