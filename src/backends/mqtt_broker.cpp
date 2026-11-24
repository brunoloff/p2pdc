#include "mqtt_broker.hpp"

namespace p2pdc::detail {
namespace {
constexpr std::uint16_t keep_alive_seconds = 60;
// A lookup ends when the broker has sent nothing for this long after acknowledging it.
constexpr std::chrono::milliseconds quiet{400};
// The largest packet: four of the largest values limits allow, with room for headers.
constexpr std::size_t max_packet_bytes = 4 * 32768 + 4096;

std::string topic_of(const std::string &hex) { return "p2pdc/v1/" + hex; }

// Calls done with the refusal an acknowledgment carries, if any.
auto acknowledged(RecordClient::Done done) {
    return [done = std::move(done)](std::variant<MqttAck, BackendFailure> reply) {
        if (auto *failed = std::get_if<BackendFailure>(&reply))
            done(*failed);
        else if (const auto reason = std::get<MqttAck>(reply).reason; reason >= 0x80)
            done(mqtt_refusal(reason));
        else
            done(std::nullopt);
    };
}
} // namespace

struct MqttBroker::WatchState {
    std::uint64_t id;
    std::string hex;
    Listener listener;
    StoreLimits limits;
    RateGate gate{max_records_per_second};
    bool joined = false;
    bool readied = false;
    bool ended = false;
};

std::shared_ptr<MqttBroker> MqttBroker::create(std::shared_ptr<BackendLoop> loop, std::string url) {
    auto broker = std::shared_ptr<MqttBroker>(new MqttBroker(loop));
    const std::weak_ptr<MqttBroker> weak = broker;
    broker->channel_ = SocketChannel::create(std::move(loop), std::move(url), {"mqtt"},
                                             {[weak](SocketMessage data) {
                                                  if (auto self = weak.lock())
                                                      self->receive(std::move(data));
                                              },
                                              [weak] {
                                                  if (auto self = weak.lock())
                                                      self->lost(BackendFailure::Transient);
                                              }});
    return broker;
}

MqttBroker::MqttBroker(std::shared_ptr<BackendLoop> loop)
    : loop_(loop), replies_(std::move(loop)) {}

std::uint16_t MqttBroker::packet_id() {
    next_packet_ = static_cast<std::uint16_t>(next_packet_ % 65535 + 1);
    return next_packet_;
}

void MqttBroker::connected(Done done) {
    if (session_) {
        done(std::nullopt);
        return;
    }
    connecting_.push_back(std::move(done));
    if (connecting_.size() > 1)
        return;
    channel_->ready([self = shared_from_this()](std::optional<BackendFailure> failure) {
        if (failure) {
            self->session_ready(failure);
            return;
        }
        self->decoder_.emplace(max_packet_bytes);
        self->request(mqtt_connect("p2pdc" + random_id().substr(0, 16), keep_alive_seconds),
                      "connack", [self](auto result) { self->session_ready(result); });
    });
}

void MqttBroker::session_ready(std::optional<BackendFailure> failure) {
    if (!failure) {
        session_ = true;
        ping();
    }
    for (auto &done : std::exchange(connecting_, {}))
        done(failure);
}

// Arms the next keep-alive ping, sent at half the keep-alive interval.
void MqttBroker::ping() {
    loop_->cancel(pinger_);
    pinger_ =
        loop_->post_after(std::chrono::seconds(keep_alive_seconds / 2), [weak = weak_from_this()] {
            auto self = weak.lock();
            if (!self || !self->session_)
                return;
            self->channel_->send(mqtt_ping());
            self->ping();
        });
}

void MqttBroker::request(Bytes packet, const std::string &reply, Done done) {
    replies_.wait(reply, acknowledged(std::move(done)));
    if (!channel_->send(packet))
        replies_.fail(reply, BackendFailure::Transient);
}

void MqttBroker::put(const RecordKey &key, Bytes value, StoreLimits limits, Done done) {
    if (value.size() > limits.max_value_bytes)
        throw std::invalid_argument("oversized MQTT payload");
    const auto topic = topic_of(to_hex(key)) + "/" + random_id();
    connected([self = shared_from_this(), topic, value = std::move(value),
               done = std::move(done)](std::optional<BackendFailure> failure) mutable {
        if (failure) {
            done(failure);
            return;
        }
        const auto id = self->packet_id();
        self->request(mqtt_publish(topic, id, value, record_lifetime.count()),
                      "puback:" + std::to_string(id), std::move(done));
    });
}

// Adds a lookup or watch to the key, then subscribes when needed: again for a lookup, so that
// the broker replays stored values, which may arrive with its acknowledgment.
void MqttBroker::join(const std::string &hex, std::function<void(KeyEntry &)> add, bool lookup,
                      Done done) {
    connected([self = shared_from_this(), hex, add = std::move(add), lookup,
               done = std::move(done)](std::optional<BackendFailure> failure) {
        if (failure) {
            done(failure);
            return;
        }
        auto &entry = self->keys_[hex];
        ++entry.users;
        add(entry);
        if (entry.users > 1 && !lookup) {
            done(std::nullopt);
            return;
        }
        const auto id = self->packet_id();
        self->request(mqtt_subscribe(id, topic_of(hex) + "/+", lookup),
                      "suback:" + std::to_string(id), std::move(done));
    });
}

void MqttBroker::leave(const std::string &hex, std::uint64_t target) {
    const auto found = keys_.find(hex);
    if (found == keys_.end())
        return;
    auto &entry = found->second;
    if (!entry.lookups.erase(target) && !entry.watches.erase(target))
        return;
    if (--entry.users > 0)
        return;
    keys_.erase(found);
    channel_->send(mqtt_unsubscribe(packet_id(), topic_of(hex) + "/+"));
}

void MqttBroker::get(const RecordKey &key, StoreLimits limits, bool by_content, Found found) {
    const auto hex = to_hex(key);
    const auto target = ++next_target_;
    const auto reply = "lookup:" + std::to_string(target);
    auto collector = std::make_shared<RecordCollector>(limits, by_content);
    auto timer = std::make_shared<BackendLoop::TimerId>(0);
    auto self = shared_from_this();
    const auto rearm = [self, timer, reply] {
        self->loop_->cancel(*timer);
        *timer =
            self->loop_->post_after(quiet, [self, reply] { self->replies_.settle(reply, {}); });
    };
    replies_.wait(reply,
                  [self, hex, target, timer, collector, found = std::move(found)](auto result) {
                      self->loop_->cancel(*timer);
                      self->leave(hex, target);
                      if (auto *failed = std::get_if<BackendFailure>(&result))
                          found(*failed);
                      else
                          found(collector->take());
                  });
    const auto add = [target, collector, limits, rearm](KeyEntry &entry) {
        entry.lookups.emplace(target, [collector, limits, rearm](const BackendRecord &record) {
            if (record.bytes.size() <= limits.max_value_bytes)
                collector->add(record);
            rearm();
        });
    };
    join(hex, add, true, [self, reply, timer, rearm](std::optional<BackendFailure> failure) {
        if (failure)
            self->replies_.fail(reply, *failure);
        else if (*timer == 0)
            rearm();
    });
}

RecordClient::WatchId MqttBroker::listen(const RecordKey &key, StoreLimits limits,
                                         Listener listener) {
    auto watch = std::make_shared<WatchState>(
        WatchState{++next_target_, to_hex(key), std::move(listener), limits});
    watches_.emplace(watch->id, watch);
    auto self = shared_from_this();
    const auto add = [weak_self = weak_from_this(), watch](KeyEntry &entry) {
        watch->joined = true;
        const std::weak_ptr<WatchState> weak = watch;
        entry.watches.emplace(
            watch->id, WatchTarget{[weak](const BackendRecord &record) {
                                       auto state = weak.lock();
                                       if (state && !state->ended &&
                                           record.bytes.size() <= state->limits.max_value_bytes &&
                                           state->gate.admit())
                                           state->listener.record(record);
                                   },
                                   [weak_self, weak](BackendFailure failure) {
                                       auto owner = weak_self.lock();
                                       if (auto state = weak.lock(); owner && state)
                                           owner->end_watch(state, failure, true);
                                   }});
    };
    join(watch->hex, add, false, [self, watch](std::optional<BackendFailure> failure) {
        if (failure)
            self->end_watch(watch, failure, true);
        else if (!watch->ended && !std::exchange(watch->readied, true))
            watch->listener.ready(true);
    });
    return watch->id;
}

void MqttBroker::end_watch(const std::shared_ptr<WatchState> &watch,
                           std::optional<BackendFailure> failure, bool tell) {
    if (std::exchange(watch->ended, true))
        return;
    watches_.erase(watch->id);
    if (watch->joined)
        leave(watch->hex, watch->id);
    if (!tell)
        return;
    if (!std::exchange(watch->readied, true))
        watch->listener.ready(false);
    watch->listener.ended(failure);
}

void MqttBroker::unlisten(WatchId watch) {
    if (const auto found = watches_.find(watch); found != watches_.end())
        end_watch(found->second, std::nullopt, false);
}

void MqttBroker::receive(SocketMessage data) {
    if (!decoder_)
        return;
    const auto *text = std::get_if<std::string>(&data);
    const auto bytes = text ? std::as_bytes(std::span(text->data(), text->size()))
                            : ByteView(std::get<Bytes>(data));
    auto packets = decoder_->push(bytes);
    if (!packets) {
        channel_->drop();
        lost(BackendFailure::InvalidStream);
        return;
    }
    for (const auto &packet : *packets) {
        const auto type = static_cast<MqttType>(packet.type);
        if (type == MqttType::Publish) {
            published(packet);
            continue;
        }
        const auto ack = mqtt_ack(packet.type, packet.body);
        if (!ack)
            continue;
        if (type == MqttType::Connack)
            replies_.settle("connack", *ack);
        else if (type == MqttType::Puback || type == MqttType::Suback)
            replies_.settle((type == MqttType::Puback ? "puback:" : "suback:") +
                                std::to_string(ack->packet_id),
                            *ack);
    }
}

void MqttBroker::published(const MqttPacket &packet) {
    auto message = mqtt_message(packet.flags, packet.body);
    if (!message)
        return;
    if (message->packet_id)
        channel_->send(mqtt_puback(*message->packet_id));
    const auto prefix = std::string("p2pdc/v1/");
    if (!message->topic.starts_with(prefix) || message->payload.empty())
        return;
    const auto rest = message->topic.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash + 1 == rest.size())
        return;
    const auto found = keys_.find(rest.substr(0, slash));
    if (found == keys_.end())
        return;
    const BackendRecord record{std::move(message->payload), rest.substr(slash + 1)};
    std::vector<Target> targets;
    for (const auto &[unused, target] : found->second.lookups)
        targets.push_back(target);
    if (!message->retained)
        for (const auto &[unused, target] : found->second.watches)
            targets.push_back(target.record);
    for (const auto &target : targets)
        target(record);
}

void MqttBroker::close() {
    channel_->close();
    lost(BackendFailure::Transient);
}

void MqttBroker::lost(BackendFailure failure) {
    loop_->cancel(pinger_);
    session_ = false;
    decoder_.reset();
    auto keys = std::exchange(keys_, {});
    replies_.fail_all(failure);
    for (auto &[hex, entry] : keys)
        for (auto &[id, target] : entry.watches)
            target.lost(failure);
}

} // namespace p2pdc::detail
