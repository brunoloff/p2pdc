#pragma once

#include "native_transport.hpp"
#include "p2pdc/ice_config.h"

namespace p2pdc::detail {

struct NativeIceConfig {
    rtc::Configuration rtc;
    NativeRelayPolicy policy;
};

// Throws invalid_argument for invalid settings and never logs credentials.
[[nodiscard]] NativeIceConfig make_native_ice_config(const IceConfig &config);
// Explicit feature input permits validation independently of the loaded backend.
[[nodiscard]] NativeIceConfig make_native_ice_config(const IceConfig &config,
                                                     unsigned int features);

} // namespace p2pdc::detail
