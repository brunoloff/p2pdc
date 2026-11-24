#include "nostr_relay.hpp"

#include "nostr_event.hpp"

#include <nlohmann/json.hpp>

namespace p2pdc::detail {

std::shared_ptr<NostrRelay> NostrRelay::create(std::shared_ptr<BackendLoop> loop, std::string url) {
    auto relay = std::shared_ptr<NostrRelay>(new NostrRelay(loop));
    const std::weak_ptr<NostrRelay> weak = relay;
    relay->channel_ = SocketChannel::create(std::move(loop), std::move(url), {},
                                            {[weak](SocketMessage data) {
                                                 if (auto self = weak.lock())
                                                     self->receive(std::move(data));
                                             },
                                             [weak] {
                                                 if (auto self = weak.lock())
                                                     self->lost(BackendFailure::Transient);
                                             }});
    return relay;
}

NostrRelay::NostrRelay(std::shared_ptr<BackendLoop> loop)
    : loop_(loop), oks_(loop), eoses_(std::move(loop)) {}

void NostrRelay::put(const RecordKey &key, Bytes value, StoreLimits limits, Done done) {
    if (value.size() > limits.max_value_bytes)
        throw std::invalid_argument("oversized Nostr payload");
    auto event = nostr_event(signer_, key, value, unix_now());
    channel_->ready([self = shared_from_this(), event = std::move(event),
                     done = std::move(done)](std::optional<BackendFailure> failure) mutable {
        if (failure) {
            done(failure);
            return;
        }
        const auto id = event["id"].get<std::string>();
        self->oks_.wait("ok:" + id, [done](std::variant<Ok, BackendFailure> reply) {
            if (auto *failed = std::get_if<BackendFailure>(&reply))
                done(*failed);
            else if (auto &[accepted, message] = std::get<Ok>(reply); !accepted)
                done(nostr_refusal(message));
            else
                done(std::nullopt);
        });
        if (!self->channel_->send(nlohmann::json::array({"EVENT", event}).dump()))
            self->oks_.fail("ok:" + id, BackendFailure::Transient);
    });
}

void NostrRelay::get(const RecordKey &key, StoreLimits limits, bool by_content, Found found) {
    const auto hex = to_hex(key);
    const auto id = random_id();
    auto collector = std::make_shared<RecordCollector>(limits, by_content);
    eoses_.wait("eose:" + id,
                [self = shared_from_this(), id, collector, found = std::move(found)](auto reply) {
                    self->unsubscribe(id);
                    if (auto *failed = std::get_if<BackendFailure>(&reply))
                        found(*failed);
                    else
                        found(collector->take());
                });
    Subscription subscription{
        [collector, hex, limits](const nlohmann::json &event) {
            if (auto record = nostr_record(event, hex, limits.max_value_bytes, unix_now()))
                collector->add(*record);
        },
        [this, id] { eoses_.settle("eose:" + id, std::monostate{}); },
        [this, id](BackendFailure failure) { eoses_.fail("eose:" + id, failure); }};
    request(id, std::move(subscription),
            {{"kinds", {nostr_kind}}, {"#x", {hex}}, {"limit", limits.max_values}});
}

RecordClient::WatchId NostrRelay::listen(const RecordKey &key, StoreLimits limits,
                                         Listener listener) {
    const auto hex = to_hex(key);
    const auto id = random_id();
    const auto watch = ++next_watch_;
    watches_.emplace(watch, id);
    auto shared = std::make_shared<Listener>(std::move(listener));
    auto gate = std::make_shared<RateGate>(max_records_per_second);
    auto readied = std::make_shared<bool>(false);
    Subscription subscription{[shared, gate, hex, limits](const nlohmann::json &event) {
                                  auto record =
                                      nostr_record(event, hex, limits.max_value_bytes, unix_now());
                                  if (record && gate->admit())
                                      shared->record(std::move(*record));
                              },
                              [shared, readied] {
                                  if (!std::exchange(*readied, true))
                                      shared->ready(true);
                              },
                              [this, shared, readied, watch](BackendFailure failure) {
                                  watches_.erase(watch);
                                  if (!std::exchange(*readied, true))
                                      shared->ready(false);
                                  shared->ended(failure);
                              }};
    request(id, std::move(subscription),
            {{"kinds", {nostr_kind}}, {"#x", {hex}}, {"since", unix_now()}});
    return watch;
}

void NostrRelay::unlisten(WatchId watch) {
    const auto found = watches_.find(watch);
    if (found == watches_.end())
        return;
    const auto id = found->second;
    watches_.erase(found);
    unsubscribe(id);
}

// Registers the subscription now, so that it fails with the connection, and sends the REQ once
// the connection is open.
void NostrRelay::request(const std::string &id, Subscription subscription, nlohmann::json filter) {
    subscriptions_.emplace(id, std::move(subscription));
    channel_->ready([self = shared_from_this(), id, filter = std::move(filter)](auto failure) {
        const auto found = self->subscriptions_.find(id);
        if (found == self->subscriptions_.end())
            return;
        if (!failure && self->channel_->send(nlohmann::json::array({"REQ", id, filter}).dump()))
            return;
        auto closed = std::move(found->second.closed);
        self->subscriptions_.erase(found);
        closed(failure.value_or(BackendFailure::Transient));
    });
}

void NostrRelay::unsubscribe(const std::string &id) {
    if (subscriptions_.erase(id))
        channel_->send(nlohmann::json::array({"CLOSE", id}).dump());
}

void NostrRelay::receive(SocketMessage data) {
    const auto *text = std::get_if<std::string>(&data);
    const auto message = nlohmann::json::parse(
        text ? std::string_view(*text)
             : std::string_view(reinterpret_cast<const char *>(std::get<Bytes>(data).data()),
                                std::get<Bytes>(data).size()),
        nullptr, false);
    if (message.is_array() && message.size() >= 2 && message[0].is_string() &&
        message[1].is_string())
        received(message);
}

void NostrRelay::received(const nlohmann::json &message) {
    const auto &type = message[0].get_ref<const std::string &>();
    const auto &first = message[1].get_ref<const std::string &>();
    const auto text = [&](std::size_t index) {
        return message.size() > index && message[index].is_string()
                   ? message[index].get<std::string>()
                   : std::string();
    };
    if (type == "OK") {
        oks_.settle("ok:" + first, Ok{message.size() > 2 && message[2] == true, text(3)});
        return;
    }
    const auto found = subscriptions_.find(first);
    if (found == subscriptions_.end())
        return;
    if (type == "EVENT" && message.size() > 2) {
        found->second.event(message[2]);
    } else if (type == "EOSE") {
        found->second.eose();
    } else if (type == "CLOSED") {
        auto closed = std::move(found->second.closed);
        subscriptions_.erase(found);
        closed(nostr_refusal(text(2)));
    }
}

void NostrRelay::close() {
    channel_->close();
    lost(BackendFailure::Transient);
}

void NostrRelay::lost(BackendFailure failure) {
    oks_.fail_all(failure);
    eoses_.fail_all(failure);
    while (!subscriptions_.empty()) {
        auto closed = std::move(subscriptions_.begin()->second.closed);
        subscriptions_.erase(subscriptions_.begin());
        closed(failure);
    }
}

} // namespace p2pdc::detail
