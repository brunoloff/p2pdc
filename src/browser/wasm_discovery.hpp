#pragma once

#include "../discovery/advertisement_tracker.hpp"
#include "../discovery/incoming_offer_resolver.hpp"
#include "p2pdc/discovery_key.h"
#include "p2pdc/recipient_crypto.h"

#include <map>
#include <memory>

namespace p2pdc::detail {

inline constexpr std::size_t wasm_discovery_config_size = 96;
inline constexpr std::size_t wasm_peer_size = 132;

// Single serialized, preauthorized group; caller treats emitted peers as
// authenticated advertisements, not as connection authorization. Input
// credentials and notification consumers outlive calls and must not reenter.
class WasmDiscovery {
  public:
    static std::unique_ptr<WasmDiscovery> create(unsigned id, ByteView config);
    ~WasmDiscovery();
    WasmDiscovery(const WasmDiscovery &) = delete;
    WasmDiscovery &operator=(const WasmDiscovery &) = delete;
    [[nodiscard]] bool key(std::span<std::byte> output) const;
    [[nodiscard]] bool window_key(std::uint64_t now, std::span<std::byte> output) const;
    [[nodiscard]] bool self(std::span<std::byte> output) const;
    [[nodiscard]] bool publish(std::uint64_t now, std::span<std::byte> output,
                               bool windowed = false);
    [[nodiscard]] int receive(ByteView wire, std::uint64_t now);
    [[nodiscard]] bool sweep(std::uint64_t now);
    [[nodiscard]] bool current(unsigned handle, std::uint64_t now, std::span<std::byte> output);
    // 0 ignored, 1 admitted, 2 failure, 3 unknown peer, 4 authenticated non-offer,
    // 5 verified offer inspected without admission.
    // The caller must separately ensure publication succeeded and obtain consent.
    [[nodiscard]] int incoming(WasmDiscovery &watch, ByteView wire, std::uint64_t now,
                               bool inspect_only, std::span<std::byte> output);
    [[nodiscard]] bool healthy() const { return healthy_; }

  private:
    struct Secret {
        explicit Secret(ByteView value);
        ~Secret();
        std::array<std::byte, 32> bytes{};
    };
    WasmDiscovery(unsigned id, Identity identity, RecipientKeyPair box, ByteView capability);
    [[nodiscard]] bool emit(unsigned handle, unsigned event, const Advertisement &value);
    [[nodiscard]] bool install(const Advertisement &value, AdvertisementEvent event);
    [[nodiscard]] bool expired(const Advertisement &value);
    [[nodiscard]] bool time(std::uint64_t now);
    unsigned id_;
    Identity identity_;
    RecipientKeyPair box_;
    Secret capability_;
    DiscoveryKey key_{};
    AdvertisementFields local_{};
    AdvertisementTracker tracker_{128, 300, 30};
    IncomingOfferResolver incoming_{{32, 60, 300, 30, 32768 - signaling_wire_overhead, 63}};
    std::map<unsigned, Advertisement> handles_;
    unsigned next_handle_ = 1;
    std::uint64_t publications_ = 0;
    unsigned window_publications_ = 0;
    std::optional<std::uint64_t> publication_window_;
    std::optional<bool> publication_mode_;
    std::optional<std::uint64_t> last_time_;
    bool healthy_ = true;
};

} // namespace p2pdc::detail
