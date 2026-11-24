#pragma once

// Shared by the files of PeerJsSwitchboard (peerjs_switchboard*.cpp).
#include "peerjs_switchboard.hpp"

namespace p2pdc::detail {

// One watch of this store: on the key's host, here or remote.
struct PeerJsSwitchboard::Watch {
    std::string sub;
    std::string hex;
    StoreLimits limits;
    Listener listener;
    RateGate gate{max_records_per_second};
    BackendLoop::TimerId renew = 0;
    std::function<void()> renewal = {}; // arms the next renewal
    bool readied = false;
    bool ended = false;

    // A value the host has or was told of; junk from an untrusted host is dropped.
    void deliver(const std::string &id, const std::string &data) {
        if (ended || !gate.admit())
            return;
        if (auto bytes = decode_base64(data, limits.max_value_bytes))
            listener.record({std::move(*bytes), id});
    }
};

namespace peerjs {
inline constexpr std::size_t max_host_links = 128;
// Base64 of the largest value a store puts.
inline constexpr std::size_t max_data_chars = 4 * ((32768 + 2) / 3);
inline constexpr std::chrono::milliseconds renew_interval{30000}, sweep_interval{30000},
    idle_host_link{60000};

inline std::string host_id(const std::string &hex) { return "p2pdc" + hex; }
inline std::string text(const nlohmann::json &object, const char *name) {
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}
// A record protocol message: "p2pdc": 1.
inline bool ours(const nlohmann::json &payload) {
    const auto found = payload.find("p2pdc");
    return found != payload.end() && found->is_number_integer() && *found == 1;
}
inline nlohmann::json values_json(const std::vector<KeyHost::Value> &values) {
    auto out = nlohmann::json::array();
    for (const auto &[id, data] : values)
        out.push_back({{"id", id}, {"data", data}});
    return out;
}
} // namespace peerjs

} // namespace p2pdc::detail
