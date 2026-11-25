#include "p2pdc/discovery_backends.h"

#include "discovery_backends_json.hpp"

#include <algorithm>
#include <stdexcept>

namespace p2pdc {
namespace {

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> result;
    while (!text.empty()) {
        const auto start = text.find_first_not_of(" \t");
        if (start == std::string_view::npos)
            break;
        text.remove_prefix(start);
        const auto end = std::min(text.find_first_of(" \t"), text.size());
        result.push_back(text.substr(0, end));
        text.remove_prefix(end);
    }
    return result;
}

// scheme://host[:port][/path][?query], no credentials, fragment or control characters. The
// browser side checks URLs fully; this keeps settings from carrying what it would refuse.
bool valid_url(std::string_view url, std::string_view secure, std::string_view plain) {
    if (url.size() > 2048 || std::ranges::any_of(url, [](char c) {
            return static_cast<unsigned char>(c) <= ' ' || c == '"' || c == '\\';
        }))
        return false;
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
        return false;
    const auto scheme = url.substr(0, scheme_end);
    auto rest = url.substr(scheme_end + 3);
    const auto authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        url.find('#') != std::string_view::npos)
        return false;
    const auto host = authority.starts_with('[') ? authority.substr(0, authority.find(']') + 1)
                                                 : authority.substr(0, authority.find(':'));
    const bool local = host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    return !host.empty() && (scheme == secure || (local && scheme == plain));
}

struct Validator {
    bool operator()(const OpenDhtProxies &value) const {
        return !value.urls.empty() && value.urls.size() <= max_opendht_proxies &&
               std::ranges::all_of(value.urls,
                                   [](const auto &url) { return valid_url(url, "https", "http"); });
    }
    bool operator()(const NostrRelay &value) const { return valid_url(value.url, "wss", "ws"); }
    bool operator()(const MqttBroker &value) const { return valid_url(value.url, "wss", "ws"); }
    bool operator()(const PeerJsServer &value) const {
        return valid_url(value.url, "wss", "ws") && !value.key.empty() && value.key.size() <= 256 &&
               std::ranges::none_of(value.key, [](char c) {
                   return static_cast<unsigned char>(c) <= ' ' || c == '"' || c == '\\';
               });
    }
};

std::optional<DiscoveryBackend> parsed(const std::vector<std::string_view> &parts) {
    const std::string kind(parts.front());
    if (kind == "opendht" && parts.size() >= 2)
        return OpenDhtProxies{{parts.begin() + 1, parts.end()}};
    if (kind == "nostr" && parts.size() == 2)
        return NostrRelay{std::string(parts[1])};
    if (kind == "mqtt" && parts.size() == 2)
        return MqttBroker{std::string(parts[1])};
    if (kind == "peerjs" && (parts.size() == 2 || parts.size() == 3))
        return parts.size() == 3 ? PeerJsServer{std::string(parts[1]), std::string(parts[2])}
                                 : PeerJsServer{std::string(parts[1])};
    return std::nullopt;
}

} // namespace

bool valid_discovery_backend(const DiscoveryBackend &backend) {
    return std::visit(Validator{}, backend);
}

std::optional<DiscoveryBackend> parse_discovery_backend(std::string_view text) {
    const auto parts = words(text);
    if (parts.empty())
        return std::nullopt;
    auto backend = parsed(parts);
    if (!backend || !valid_discovery_backend(*backend))
        return std::nullopt;
    return backend;
}

std::string to_string(const DiscoveryBackend &backend) {
    struct Writer {
        std::string operator()(const OpenDhtProxies &value) const {
            std::string text = "opendht";
            for (const auto &url : value.urls)
                text += " " + url;
            return text;
        }
        std::string operator()(const NostrRelay &value) const { return "nostr " + value.url; }
        std::string operator()(const MqttBroker &value) const { return "mqtt " + value.url; }
        std::string operator()(const PeerJsServer &value) const {
            return "peerjs " + value.url + " " + value.key;
        }
    };
    return std::visit(Writer{}, backend);
}

namespace detail {

void validate_discovery_backends(const std::vector<::p2pdc::DiscoveryBackend> &backends) {
    if (backends.size() > max_discovery_backends ||
        !std::ranges::all_of(backends, valid_discovery_backend))
        throw std::invalid_argument("nodes take at most 8 valid discovery backends");
}

// Valid URLs and keys hold no quote, backslash or control character, so they need no escaping.
std::string discovery_backends_json(const std::vector<::p2pdc::DiscoveryBackend> &backends) {
    if (backends.empty() || backends.size() > max_discovery_backends ||
        !std::ranges::all_of(backends, valid_discovery_backend))
        throw std::invalid_argument("browser nodes take 1 to 8 valid discovery backends");
    struct Writer {
        std::string operator()(const OpenDhtProxies &value) const {
            std::string urls;
            for (const auto &url : value.urls)
                urls += (urls.empty() ? "\"" : ",\"") + url + "\"";
            return R"({"kind":"opendht","urls":[)" + urls + "]}";
        }
        std::string operator()(const NostrRelay &value) const {
            return R"({"kind":"nostr","url":")" + value.url + "\"}";
        }
        std::string operator()(const MqttBroker &value) const {
            return R"({"kind":"mqtt","url":")" + value.url + "\"}";
        }
        std::string operator()(const PeerJsServer &value) const {
            return R"({"kind":"peerjs","url":")" + value.url + R"(","key":")" + value.key + "\"}";
        }
    };
    std::string json;
    for (const auto &backend : backends)
        json += (json.empty() ? "[" : ",") + std::visit(Writer{}, backend);
    return json + "]";
}

} // namespace detail
} // namespace p2pdc
