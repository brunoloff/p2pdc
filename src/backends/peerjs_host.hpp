#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace p2pdc::detail {

// What the host of one key keeps (docs/DISCOVERY_BACKENDS.md), a port of peerjs_host.mjs: its
// values until they expire, and who watches it. Anyone may put values, so both are capped.
class KeyHost {
  public:
    using Clock = std::chrono::steady_clock;
    struct Watcher {
        std::string client;
        std::string sub;
    };
    // The values as they travel: id and base64 data.
    using Value = std::pair<std::string, std::string>;
    static constexpr std::chrono::milliseconds watch_lease{90000}; // watchers renew every 30 s

    // Keeps a value and returns the remote watchers to tell, or nullopt when full.
    std::optional<std::vector<Watcher>> put(const std::string &id, const std::string &data,
                                            std::chrono::milliseconds lifetime);
    std::vector<Value> values();
    bool watch(const std::string &client, const std::string &sub);
    void unwatch(const std::string &client, const std::string &sub);
    void forget(const std::string &client);
    bool idle();
    // This store's own watches, by subscription.
    std::map<std::string, std::function<void(const std::string &, const std::string &)>> listeners;

  private:
    struct Record {
        std::string data;
        Clock::time_point expires;
    };
    struct Watching {
        Watcher watcher;
        Clock::time_point expires;
    };
    void sweep();
    std::map<std::string, Record> records_;
    std::map<std::string, Watching> watchers_; // "client sub"
    std::size_t bytes_ = 0;
};

} // namespace p2pdc::detail
