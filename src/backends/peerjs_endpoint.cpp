#include "peerjs_endpoint.hpp"

#include "../transport/native_ice_config.hpp"

#include <nlohmann/json.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/peerconnection.hpp>

#include <iostream>
#include <regex>

namespace p2pdc::detail {
namespace {
constexpr std::size_t max_sdp_chars = 65536, max_candidate_chars = 4096;
constexpr std::size_t max_negotiations = 32, max_queued_candidates = 64;

const nlohmann::json *field(const nlohmann::json &object, const char *name) {
    if (!object.is_object())
        return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}
std::string text(const nlohmann::json &object, const char *name) {
    const auto *value = field(object, name);
    return value && value->is_string() ? value->get<std::string>() : std::string();
}
// The sdp of a description of the given type, or empty when it is not one.
std::string description(const nlohmann::json *value, const std::string &type) {
    if (!value || text(*value, "type") != type)
        return {};
    auto sdp = text(*value, "sdp");
    return sdp.size() <= max_sdp_chars ? sdp : std::string();
}
} // namespace

rtc::Configuration peerjs_rtc_configuration() {
    IceConfig ice;
    ice.servers = {StunServer{"stun.l.google.com", 19302},
                   TurnServer{"eu-0.turn.peerjs.com", 3478, "peerjs", "peerjsp"},
                   TurnServer{"us-0.turn.peerjs.com", 3478, "peerjs", "peerjsp"}};
    return make_native_ice_config(ice).rtc;
}

struct PeerJsEndpoint::Negotiation {
    std::string id;
    std::string peer;
    bool offerer;
    Connected done;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<rtc::Candidate> queued;
    bool described = false;
    BackendLoop::TimerId timer = 0;
};

std::shared_ptr<PeerJsEndpoint> PeerJsEndpoint::create(std::shared_ptr<BackendLoop> loop,
                                                       const std::string &url,
                                                       const std::string &key, std::string id,
                                                       Options options) {
    auto endpoint = std::shared_ptr<PeerJsEndpoint>(new PeerJsEndpoint(loop, std::move(options)));
    const std::weak_ptr<PeerJsEndpoint> weak = endpoint;
    auto on = [weak](auto step) {
        return [weak, step](auto &&...arguments) {
            if (auto self = weak.lock())
                step(*self, arguments...);
        };
    };
    endpoint->socket_ = PeerJsSocket::create(
        std::move(loop), url, key, std::move(id),
        {on([](PeerJsEndpoint &self, const nlohmann::json &message) { self.signal(message); }),
         on([](PeerJsEndpoint &self, const std::string &peer) { self.gone(peer); }),
         on([](PeerJsEndpoint &self, BackendFailure failure) { self.lost(failure); })});
    return endpoint;
}

PeerJsEndpoint::PeerJsEndpoint(std::shared_ptr<BackendLoop> loop, Options options)
    : loop_(std::move(loop)), options_(std::move(options)) {}

PeerJsEndpoint::~PeerJsEndpoint() {
    for (const auto &[id, negotiation] : negotiations_) {
        loop_->cancel(negotiation->timer);
        negotiation->pc->resetCallbacks();
        negotiation->pc->close();
    }
}

// A peer connection whose events come back to this endpoint on the loop, by connection ID.
std::shared_ptr<PeerJsEndpoint::Negotiation>
PeerJsEndpoint::negotiate(const std::string &peer, const std::string &id, Connected done) {
    auto negotiation = std::make_shared<Negotiation>(
        Negotiation{id, peer, false, std::move(done), nullptr, nullptr, {}});
    negotiation->pc = std::make_shared<rtc::PeerConnection>(options_.rtc);
    const std::weak_ptr<PeerJsEndpoint> weak = weak_from_this();
    const auto loop = loop_;
    const auto later = [loop, weak](auto step) {
        loop->post([weak, step] {
            if (auto self = weak.lock())
                step(*self);
        });
    };
    negotiation->pc->onLocalDescription([later, id](rtc::Description value) {
        later([id, sdp = std::string(value), type = value.typeString()](PeerJsEndpoint &self) {
            self.local_description(id, sdp, type);
        });
    });
    negotiation->pc->onLocalCandidate([later, id](rtc::Candidate value) {
        later([id, value](PeerJsEndpoint &self) { self.local_candidate(id, value); });
    });
    negotiation->pc->onStateChange([later, id](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Failed)
            later([id](PeerJsEndpoint &self) { self.fail(id, {BackendFailure::Transient}); });
    });
    negotiation->timer = loop_->post_after(backend_timeout, [weak, id] {
        if (auto self = weak.lock())
            self->fail(id, {BackendFailure::Transient});
    });
    negotiations_.emplace(id, negotiation);
    return negotiation;
}

void PeerJsEndpoint::local_candidate(const std::string &id, const rtc::Candidate &value) {
    const auto found = negotiations_.find(id);
    if (found == negotiations_.end() || value.candidate().empty())
        return;
    socket_->send(
        "CANDIDATE", found->second->peer,
        {{"candidate",
          {{"candidate", value.candidate()}, {"sdpMid", value.mid()}, {"sdpMLineIndex", 0}}},
         {"type", "data"},
         {"connectionId", id}});
}

void PeerJsEndpoint::connect(const std::string &peer, Connected done) {
    socket_->register_id([self = shared_from_this(), peer, done = std::move(done)](auto state) {
        if (const auto *failure = std::get_if<BackendFailure>(&state)) {
            done(PeerJsError{*failure});
            return;
        }
        if (std::get<PeerJsSocket::Registration>(state) != PeerJsSocket::Registration::Open) {
            done(PeerJsError{BackendFailure::Rejected});
            return;
        }
        if (self->negotiations_.size() >= max_negotiations) {
            done(PeerJsError{BackendFailure::Transient});
            return;
        }
        const auto id = "dc_" + random_id();
        auto negotiation = self->negotiate(peer, id, done);
        negotiation->offerer = true;
        self->offer(negotiation);
    });
}

void PeerJsEndpoint::offer(const std::shared_ptr<Negotiation> &negotiation) {
    const auto id = negotiation->id;
    try {
        negotiation->channel = negotiation->pc->createDataChannel(id); // ordered and reliable
        const std::weak_ptr<PeerJsEndpoint> weak = weak_from_this();
        negotiation->channel->onOpen(
            [loop = loop_, weak, id, channel = std::weak_ptr(negotiation->channel)] {
                loop->post([weak, id, channel] {
                    auto self = weak.lock();
                    if (auto open = channel.lock(); self && open)
                        self->opened(id, open);
                });
            });
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: cannot open a PeerJS data connection: " << error.what() << '\n';
        fail(id, {BackendFailure::Transient});
    }
}

void PeerJsEndpoint::local_description(const std::string &id, const std::string &sdp,
                                       const std::string &type) {
    const auto found = negotiations_.find(id);
    if (found == negotiations_.end())
        return;
    const auto &negotiation = found->second;
    nlohmann::json payload{
        {"sdp", {{"type", type}, {"sdp", sdp}}}, {"type", "data"}, {"connectionId", id}};
    if (negotiation->offerer && type == "offer") {
        payload.update({{"label", id}, {"reliable", true}, {"serialization", "raw"}});
        if (!socket_->send("OFFER", negotiation->peer, std::move(payload)))
            fail(id, {BackendFailure::Transient});
    } else if (!negotiation->offerer && type == "answer") {
        if (!socket_->send("ANSWER", negotiation->peer, std::move(payload)))
            fail(id, {BackendFailure::Transient});
    }
}

void PeerJsEndpoint::signal(const nlohmann::json &message) {
    const auto *payload = field(message, "payload");
    static const std::regex connection_id(R"([\w-]{1,64})");
    if (!payload || text(*payload, "type") != "data" ||
        !std::regex_match(text(*payload, "connectionId"), connection_id))
        return;
    const auto id = text(*payload, "connectionId"), type = text(message, "type");
    const auto src = text(message, "src");
    const auto found = negotiations_.find(id);
    if (type == "OFFER") {
        if (found == negotiations_.end())
            answer(src, *payload);
        return;
    }
    if (found == negotiations_.end() || found->second->peer != src)
        return;
    if (type == "ANSWER") {
        const auto sdp = description(field(*payload, "sdp"), "answer");
        if (sdp.empty())
            fail(id, {BackendFailure::Transient});
        else
            described(id, sdp, "answer");
    } else if (const auto *value = field(*payload, "candidate")) {
        candidate(id, *value);
    }
}

// Accepts an offer when an incoming handler is set and there is room; whoever offered learns
// of a failure from its own side.
void PeerJsEndpoint::answer(const std::string &peer, const nlohmann::json &payload) {
    if (!options_.incoming || negotiations_.size() >= max_negotiations ||
        links_.size() >= options_.max_links)
        return;
    const auto sdp = description(field(payload, "sdp"), "offer");
    if (sdp.empty())
        return;
    const auto id = text(payload, "connectionId");
    auto negotiation = negotiate(peer, id, [incoming = options_.incoming](auto result) {
        if (auto *link = std::get_if<Link>(&result))
            incoming(*link);
    });
    negotiation->offerer = false;
    const std::weak_ptr<PeerJsEndpoint> weak = weak_from_this();
    negotiation->pc->onDataChannel(
        [loop = loop_, weak, id](std::shared_ptr<rtc::DataChannel> channel) {
            const auto open = [loop, weak, id, channel] {
                loop->post([weak, id, channel] {
                    if (auto self = weak.lock())
                        self->opened(id, channel);
                });
            };
            channel->onOpen(open);
            if (channel->isOpen())
                open();
        });
    described(id, sdp, "offer");
}

void PeerJsEndpoint::described(const std::string &id, const std::string &sdp,
                               const std::string &type) {
    const auto found = negotiations_.find(id);
    if (found == negotiations_.end())
        return;
    auto negotiation = found->second;
    try {
        negotiation->pc->setRemoteDescription(rtc::Description(sdp, type));
        negotiation->described = true;
        for (auto &queued : std::exchange(negotiation->queued, {}))
            negotiation->pc->addRemoteCandidate(std::move(queued));
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: invalid PeerJS " << type << ": " << error.what() << '\n';
        fail(id, {BackendFailure::Transient});
    }
}

void PeerJsEndpoint::candidate(const std::string &id, const nlohmann::json &value) {
    auto line = text(value, "candidate");
    if (line.empty() || line.size() > max_candidate_chars)
        return;
    auto mid = text(value, "sdpMid");
    const auto &negotiation = negotiations_.at(id);
    try {
        rtc::Candidate candidate(std::move(line), mid.empty() ? "0" : std::move(mid));
        if (negotiation->described)
            negotiation->pc->addRemoteCandidate(std::move(candidate));
        else if (negotiation->queued.size() < max_queued_candidates)
            negotiation->queued.push_back(std::move(candidate));
    } catch (const std::exception &) {
        // A candidate libdatachannel cannot use, such as an mDNS name; others may serve.
    }
}

void PeerJsEndpoint::opened(const std::string &id, std::shared_ptr<rtc::DataChannel> channel) {
    const auto found = negotiations_.find(id);
    if (found == negotiations_.end())
        return;
    auto negotiation = found->second;
    negotiations_.erase(found);
    loop_->cancel(negotiation->timer);
    auto link = PeerJsLink::create(loop_, negotiation->peer, negotiation->pc, std::move(channel));
    link->released = [weak = weak_from_this(), raw = link.get()] {
        if (auto self = weak.lock())
            std::erase_if(self->links_, [raw](const Link &value) { return value.get() == raw; });
    };
    links_.insert(link);
    negotiation->done(link);
}

void PeerJsEndpoint::fail(const std::string &id, PeerJsError error) {
    const auto found = negotiations_.find(id);
    if (found == negotiations_.end())
        return;
    auto negotiation = found->second;
    negotiations_.erase(found);
    loop_->cancel(negotiation->timer);
    negotiation->pc->resetCallbacks();
    try {
        negotiation->pc->close();
    } catch (const std::exception &failure) {
        std::cerr << "p2pdc: closing a PeerJS negotiation failed: " << failure.what() << '\n';
    }
    negotiation->done(error);
}

// EXPIRE or LEAVE: peer is not on the server, so connections still being set up fail.
void PeerJsEndpoint::gone(const std::string &peer) {
    std::vector<std::string> ids;
    for (const auto &[id, negotiation] : negotiations_)
        if (negotiation->peer == peer)
            ids.push_back(id);
    for (const auto &id : ids)
        fail(id, {BackendFailure::Transient, true});
}

// Open data connections outlive the server connection; those being set up do not.
void PeerJsEndpoint::lost(BackendFailure failure) {
    while (!negotiations_.empty())
        fail(negotiations_.begin()->first, {failure});
    if (options_.closed)
        options_.closed(failure);
}

void PeerJsEndpoint::close() {
    socket_->close();
    while (!negotiations_.empty())
        fail(negotiations_.begin()->first, {BackendFailure::Transient});
    for (const auto &link : std::set<Link>(links_))
        link->close();
}

} // namespace p2pdc::detail
