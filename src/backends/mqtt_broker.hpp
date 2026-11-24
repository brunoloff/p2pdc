#pragma once

#include "mqtt_codec.hpp"
#include "pending_replies.hpp"
#include "record_client.hpp"
#include "socket_channel.hpp"

#include <map>
#include <memory>
#include <string>

namespace p2pdc::detail {

// One MQTT 5 broker over WebSocket as a record backend, a port of mqtt_broker.mjs
// (docs/DISCOVERY_BACKENDS.md). Each value is a retained, expiring message on a topic of its
// own under the key. The client has one subscription per key, shared by lookups and watches:
// messages the broker replays from storage go to lookups, live ones to both.
class MqttBroker final : public RecordClient, public std::enable_shared_from_this<MqttBroker> {
  public:
    static std::shared_ptr<MqttBroker> create(std::shared_ptr<BackendLoop> loop, std::string url);
    void put(const RecordKey &key, Bytes value, StoreLimits limits, Done done) override;
    void get(const RecordKey &key, StoreLimits limits, bool by_content, Found found) override;
    WatchId listen(const RecordKey &key, StoreLimits limits, Listener listener) override;
    void unlisten(WatchId watch) override;
    void close() override;

  private:
    using Target = std::function<void(const BackendRecord &)>;
    struct WatchTarget {
        Target record;
        std::function<void(BackendFailure)> lost;
    };
    struct KeyEntry {
        std::size_t users = 0;
        std::map<std::uint64_t, Target> lookups;
        std::map<std::uint64_t, WatchTarget> watches;
    };
    explicit MqttBroker(std::shared_ptr<BackendLoop> loop);
    void connected(Done done);
    void session_ready(std::optional<BackendFailure> failure);
    void request(Bytes packet, const std::string &reply, Done done);
    struct WatchState;
    void join(const std::string &hex, std::function<void(KeyEntry &)> add, bool lookup, Done done);
    void leave(const std::string &hex, std::uint64_t target);
    // By value: the watch may be the entry it erases.
    void end_watch(std::shared_ptr<WatchState> watch, std::optional<BackendFailure> failure,
                   bool tell);
    void ping();
    std::uint16_t packet_id();
    void receive(SocketMessage data);
    void published(const MqttPacket &packet);
    void lost(BackendFailure failure);

    std::shared_ptr<BackendLoop> loop_;
    std::shared_ptr<SocketChannel> channel_;
    PendingReplies<MqttAck> replies_;
    std::optional<MqttDecoder> decoder_;
    bool session_ = false;
    std::vector<Done> connecting_;
    BackendLoop::TimerId pinger_ = 0;
    std::uint16_t next_packet_ = 1;
    std::map<std::string, KeyEntry> keys_;
    std::map<std::uint64_t, std::shared_ptr<WatchState>> watches_;
    std::uint64_t next_target_ = 0;
};

} // namespace p2pdc::detail
