#include "peerjs_host.hpp"

namespace p2pdc::detail {
namespace {
constexpr std::size_t max_records = 256, max_record_bytes = 1024 * 1024, max_watchers = 256;
}

void KeyHost::sweep() {
    const auto now = Clock::now();
    std::erase_if(records_, [&](const auto &entry) {
        if (entry.second.expires > now)
            return false;
        bytes_ -= entry.second.data.size();
        return true;
    });
    std::erase_if(watchers_, [&](const auto &entry) { return entry.second.expires <= now; });
}

std::optional<std::vector<KeyHost::Watcher>>
KeyHost::put(const std::string &id, const std::string &data, std::chrono::milliseconds lifetime) {
    sweep();
    const auto previous = records_.find(id);
    const auto bytes =
        bytes_ - (previous == records_.end() ? 0 : previous->second.data.size()) + data.size();
    if ((previous == records_.end() && records_.size() >= max_records) || bytes > max_record_bytes)
        return std::nullopt;
    records_[id] = Record{data, Clock::now() + lifetime};
    bytes_ = bytes;
    for (const auto &[sub, listener] : decltype(listeners)(listeners))
        listener(id, data);
    std::vector<Watcher> tell;
    for (const auto &[name, watching] : watchers_)
        tell.push_back(watching.watcher);
    return tell;
}

std::vector<KeyHost::Value> KeyHost::values() {
    sweep();
    std::vector<Value> out;
    for (const auto &[id, record] : records_)
        out.emplace_back(id, record.data);
    return out;
}

bool KeyHost::watch(const std::string &client, const std::string &sub) {
    sweep();
    const auto name = client + " " + sub;
    if (!watchers_.contains(name) && watchers_.size() >= max_watchers)
        return false;
    watchers_[name] = Watching{{client, sub}, Clock::now() + watch_lease};
    return true;
}

void KeyHost::unwatch(const std::string &client, const std::string &sub) {
    watchers_.erase(client + " " + sub);
}

void KeyHost::forget(const std::string &client) {
    std::erase_if(watchers_,
                  [&](const auto &entry) { return entry.second.watcher.client == client; });
}

bool KeyHost::idle() {
    sweep();
    return records_.empty() && watchers_.empty() && listeners.empty();
}

} // namespace p2pdc::detail
