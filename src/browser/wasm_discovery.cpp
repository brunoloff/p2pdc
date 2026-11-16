#include "wasm_discovery.hpp"
#include "../discovery/discovery_window.hpp"

#include <emscripten.h>
#include <sodium.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace p2pdc;
using namespace p2pdc::detail;

EM_JS(int, discovery_event,
      (unsigned id, unsigned handle, unsigned event, const std::byte *data, unsigned size), {
          try {
              return Module.p2pdcDiscoveryBridge.event(id, handle, event,
                                                       HEAPU8.slice(data, data + size))
                         ? 1
                         : 0;
          } catch (error) {
              console.error("p2pdc: discovery event delivery failed");
              return 0;
          }
      });

namespace p2pdc::detail {
namespace {
template <class T> void append(Bytes &output, const T &value) {
    output.insert(output.end(), value.begin(), value.end());
}
void integer(Bytes &output, std::uint64_t value) {
    for (unsigned shift = 64; shift > 0; shift -= 8)
        output.push_back(static_cast<std::byte>((value >> (shift - 8)) & 255U));
}
Bytes pack(const Advertisement &value) {
    Bytes output;
    output.reserve(wasm_peer_size);
    append(output, value.signing_public_key);
    append(output, value.fields.recipient_public_key);
    append(output, value.fields.instance_id);
    append(output, value.fields.signaling_inbox);
    integer(output, value.fields.sequence);
    integer(output, value.fields.features);
    integer(output, value.fields.issued_at);
    integer(output, value.fields.expires_at);
    return output;
}
template <class T> void random_nonzero(T &value) {
    do {
        randombytes_buf(value.data(), value.size());
    } while (
        std::all_of(value.begin(), value.end(), [](auto byte) { return byte == std::byte{}; }));
}
bool same_peer(const Advertisement &a, const Advertisement &b) {
    return a.signing_public_key == b.signing_public_key &&
           a.fields.instance_id == b.fields.instance_id;
}
} // namespace

WasmDiscovery::Secret::Secret(ByteView value) {
    if (value.size() != bytes.size())
        throw std::invalid_argument("invalid capability");
    std::copy(value.begin(), value.end(), bytes.begin());
}
WasmDiscovery::Secret::~Secret() { sodium_memzero(bytes.data(), bytes.size()); }
WasmDiscovery::WasmDiscovery(unsigned id, Identity identity, RecipientKeyPair box,
                             ByteView capability)
    : id_(id), identity_(std::move(identity)), box_(std::move(box)), capability_(capability) {
    auto result = derive_discovery_key(capability_.bytes);
    if (!std::holds_alternative<DiscoveryKey>(result))
        throw std::runtime_error("discovery key failed");
    key_ = std::get<DiscoveryKey>(result);
    local_.recipient_public_key = box_.public_key();
    random_nonzero(local_.instance_id);
    random_nonzero(local_.signaling_inbox);
}
WasmDiscovery::~WasmDiscovery() = default;
std::unique_ptr<WasmDiscovery> WasmDiscovery::create(unsigned id, ByteView config) {
    if (!id || config.size() != wasm_discovery_config_size)
        throw std::invalid_argument("invalid config");
    auto identity = Identity::import_seed(config.first(32));
    auto box = RecipientKeyPair::import_secret_key(config.subspan(32, 32));
    if (!std::holds_alternative<Identity>(identity) ||
        !std::holds_alternative<RecipientKeyPair>(box))
        throw std::invalid_argument("credential import failed");
    return std::unique_ptr<WasmDiscovery>(
        new WasmDiscovery(id, std::move(std::get<Identity>(identity)),
                          std::move(std::get<RecipientKeyPair>(box)), config.subspan(64, 32)));
}
bool WasmDiscovery::key(std::span<std::byte> output) const {
    if (!healthy_ || output.size() != key_.size())
        return false;
    std::copy(key_.begin(), key_.end(), output.begin());
    return true;
}
bool WasmDiscovery::window_key(std::uint64_t now, std::span<std::byte> output) const {
    if (!healthy_ || output.size() != key_.size())
        return false;
    auto result = derive_discovery_window_key(capability_.bytes, discovery_window_for(now));
    if (!std::holds_alternative<DiscoveryKey>(result))
        throw std::runtime_error("discovery window key failed");
    const auto &value = std::get<DiscoveryKey>(result);
    std::copy(value.begin(), value.end(), output.begin());
    return true;
}
bool WasmDiscovery::self(std::span<std::byte> output) const {
    if (!healthy_ || output.size() != 100)
        return false;
    const Advertisement value{identity_.public_key(), local_};
    const auto packed = pack(value);
    std::copy_n(packed.begin(), output.size(), output.begin());
    return true;
}
bool WasmDiscovery::time(std::uint64_t now) {
    if (last_time_ && now < *last_time_) {
        healthy_ = false;
        return false;
    }
    last_time_ = now;
    return true;
}
bool WasmDiscovery::publish(std::uint64_t now, std::span<std::byte> output, bool windowed) {
    if (!healthy_ || output.size() != advertisement_wire_size || !time(now) ||
        (publication_mode_ && *publication_mode_ != windowed) ||
        publications_ == std::numeric_limits<std::uint64_t>::max() ||
        now > std::numeric_limits<std::uint64_t>::max() - 120) {
        healthy_ = false;
        return false;
    }
    const auto window = discovery_window_for(now);
    if (windowed && (!publication_window_ || *publication_window_ != window)) {
        publication_window_ = window;
        window_publications_ = 0;
    }
    if (windowed ? window_publications_ >= 32 : publications_ >= 32) {
        healthy_ = false;
        return false;
    }
    publication_mode_ = windowed;
    local_.issued_at = now;
    local_.expires_at = now + 120;
    local_.sequence = publications_;
    auto encoded = encode_advertisement(identity_, local_, capability_.bytes, {now, 300, 30});
    if (!std::holds_alternative<Bytes>(encoded)) {
        healthy_ = false;
        return false;
    }
    const auto &wire = std::get<Bytes>(encoded);
    std::copy(wire.begin(), wire.end(), output.begin());
    ++publications_;
    ++window_publications_;
    return true;
}
bool WasmDiscovery::emit(unsigned handle, unsigned event, const Advertisement &value) {
    auto data = pack(value);
    if (!discovery_event(id_, handle, event, data.data(), data.size())) {
        healthy_ = false;
        return false;
    }
    return true;
}
bool WasmDiscovery::expired(const Advertisement &value) {
    for (auto it = handles_.begin(); it != handles_.end(); ++it) {
        if (same_peer(it->second, value)) {
            const auto handle = it->first;
            handles_.erase(it);
            return emit(handle, 2, value);
        }
    }
    healthy_ = false;
    return false;
}
bool WasmDiscovery::sweep(std::uint64_t now) {
    if (!healthy_ || !time(now))
        return false;
    auto result = tracker_.sweep(now);
    if (std::holds_alternative<AdvertisementSweepError>(result)) {
        healthy_ = false;
        return false;
    }
    for (const auto &value : std::get<std::vector<Advertisement>>(result))
        if (!expired(value))
            return false;
    return true;
}
bool WasmDiscovery::install(const Advertisement &value, AdvertisementEvent event) {
    for (auto it = handles_.begin(); it != handles_.end(); ++it) {
        if (same_peer(it->second, value)) {
            handles_.erase(it);
            break;
        }
    }
    if (!next_handle_ || handles_.size() >= 128) {
        healthy_ = false;
        return false;
    }
    const auto handle = next_handle_++;
    handles_.emplace(handle, value);
    return emit(handle, event == AdvertisementEvent::Seen ? 0 : 1, value);
}
int WasmDiscovery::receive(ByteView wire, std::uint64_t now) {
    if (!sweep(now))
        return 2;
    auto decoded = decode_advertisement(wire, capability_.bytes, {now, 300, 30});
    if (auto *value = std::get_if<Advertisement>(&decoded)) {
        const auto event = tracker_.observe(*value, now);
        if (event == AdvertisementEvent::Seen || event == AdvertisementEvent::Updated)
            return install(*value, event) ? 0 : 2;
        // A full tracker ignores new peers rather than ending discovery.
        if (event == AdvertisementEvent::ClockRegressed) {
            healthy_ = false;
            return 2;
        }
    } else if (auto *error = std::get_if<CryptoError>(&decoded);
               error &&
               (*error == CryptoError::InitializationFailed ||
                *error == CryptoError::AllocationFailed || *error == CryptoError::InvalidIdentity ||
                *error == CryptoError::OperationFailed)) {
        healthy_ = false;
        return 2;
    }
    return 1;
}
bool WasmDiscovery::current(unsigned handle, std::uint64_t now, std::span<std::byte> output) {
    if (!healthy_ || output.size() != wasm_peer_size || !time(now))
        return false;
    const auto found = handles_.find(handle);
    if (found == handles_.end())
        return false;
    auto result = tracker_.is_current(found->second, now);
    if (std::holds_alternative<AdvertisementSweepError>(result)) {
        healthy_ = false;
        return false;
    }
    if (!std::get<bool>(result))
        return false;
    auto packed = pack(found->second);
    std::copy(packed.begin(), packed.end(), output.begin());
    return true;
}
int WasmDiscovery::incoming(WasmDiscovery &watch, ByteView wire, std::uint64_t now,
                            bool inspect_only, std::span<std::byte> output) {
    if (this == &watch || output.size() != 20 || wire.size() > 32768 || !healthy_ ||
        !watch.healthy_ || identity_.public_key() != watch.identity_.public_key() ||
        box_.public_key() != watch.box_.public_key() ||
        sodium_memcmp(capability_.bytes.data(), watch.capability_.bytes.data(), 32) != 0 ||
        !time(now) || !watch.time(now) || !publications_ || now >= local_.expires_at)
        return 2;
    auto snapshot = watch.tracker_.current_peers(now);
    if (!std::holds_alternative<std::vector<Advertisement>>(snapshot))
        return 2;
    std::optional<IncomingOfferResolver> temporary;
    if (inspect_only)
        temporary.emplace(
            IncomingOfferLimits{32, 60, 300, 30, 32768 - signaling_wire_overhead, 63});
    auto &resolver = temporary ? *temporary : incoming_;
    auto result =
        resolver.resolve(wire, box_, {identity_.public_key(), local_.instance_id},
                         capability_.bytes, std::get<std::vector<Advertisement>>(snapshot), now);
    if (auto *error = std::get_if<CryptoError>(&result))
        return *error == CryptoError::InitializationFailed ||
                       *error == CryptoError::AllocationFailed ||
                       *error == CryptoError::InvalidIdentity ||
                       *error == CryptoError::OperationFailed
                   ? 2
                   : 0;
    if (auto *error = std::get_if<IncomingOfferError>(&result)) {
        if (*error == IncomingOfferError::UnknownPeer)
            return 3;
        if (*error == IncomingOfferError::NotOffer)
            return 4;
        // Full drops the offer without evicting replay history.
        return *error == IncomingOfferError::ClockRegressed ||
                       *error == IncomingOfferError::ContextMismatch
                   ? 2
                   : 0;
    }
    const auto *offer = std::get_if<IncomingOffer>(&result);
    if (!offer)
        return 0;
    if (inspect_only)
        return 5;
    for (const auto &[handle, peer] : watch.handles_) {
        if (peer != offer->peer)
            continue;
        for (unsigned index = 0; index < 4; ++index)
            output[index] = static_cast<std::byte>((handle >> (24 - index * 8)) & 255U);
        std::copy(offer->envelope.fields.attempt_id.begin(),
                  offer->envelope.fields.attempt_id.end(), output.begin() + 4);
        return 1;
    }
    return 2;
}
} // namespace p2pdc::detail
