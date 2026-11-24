#pragma once

#include "pending_replies.hpp"
#include "record_client.hpp"
#include "schnorr.hpp"
#include "socket_channel.hpp"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <memory>
#include <string>

namespace p2pdc::detail {

// One Nostr relay as a record backend, a port of nostr_relay.mjs (docs/DISCOVERY_BACKENDS.md).
// Values are events signed with a throwaway key; every watch shares the one connection.
class NostrRelay final : public RecordClient, public std::enable_shared_from_this<NostrRelay> {
  public:
    static std::shared_ptr<NostrRelay> create(std::shared_ptr<BackendLoop> loop, std::string url);
    void put(const RecordKey &key, Bytes value, StoreLimits limits, Done done) override;
    void get(const RecordKey &key, StoreLimits limits, bool by_content, Found found) override;
    WatchId listen(const RecordKey &key, StoreLimits limits, Listener listener) override;
    void unlisten(WatchId watch) override;
    void close() override;

  private:
    struct Subscription {
        std::function<void(const nlohmann::json &)> event;
        std::function<void()> eose;
        std::function<void(BackendFailure)> closed;
    };
    using Ok = std::pair<bool, std::string>;
    explicit NostrRelay(std::shared_ptr<BackendLoop> loop);
    void request(const std::string &id, Subscription subscription, nlohmann::json filter);
    void unsubscribe(const std::string &id);
    void receive(SocketMessage data);
    void received(const nlohmann::json &message);
    void lost(BackendFailure failure);

    std::shared_ptr<BackendLoop> loop_;
    SchnorrSigner signer_;
    std::shared_ptr<SocketChannel> channel_;
    std::map<std::string, Subscription> subscriptions_;
    PendingReplies<Ok> oks_;
    PendingReplies<std::monostate> eoses_;
    std::map<WatchId, std::string> watches_;
    WatchId next_watch_ = 0;
};

} // namespace p2pdc::detail
