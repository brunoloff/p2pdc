#pragma once

namespace p2pdc::detail {

// These bits are supplied by our pinned libdatachannel patch.
inline constexpr unsigned int native_send_admission = 1U;
inline constexpr unsigned int native_force_relay = 2U;
inline constexpr unsigned int native_safe_ice_detach = 4U;
inline constexpr unsigned int native_turn_tls = 8U;

[[nodiscard]] constexpr bool supports_native_backend(unsigned int features, bool relay_only,
                                                     bool uses_turn = false) {
    const auto required = native_send_admission | native_safe_ice_detach |
                          ((relay_only || uses_turn) ? native_force_relay : 0U);
    return (features & required) == required;
}

} // namespace p2pdc::detail
