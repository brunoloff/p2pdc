#include "peerjs_socket.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <regex>

namespace p2pdc::detail {
namespace {
constexpr std::chrono::milliseconds heartbeat_interval{
    25000}; // the server drops clients silent for 90 s

bool signal_type(const std::string &type) {
    return type == "OFFER" || type == "ANSWER" || type == "CANDIDATE";
}

std::string query_value(const std::string &value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 15]);
        }
    }
    return out;
}
} // namespace

std::string peerjs_socket_url(const std::string &url, const std::string &key, const std::string &id,
                              const std::string &token) {
    const auto base = url.substr(0, url.find('?'));
    return base + "?key=" + query_value(key) + "&id=" + query_value(id) +
           "&token=" + query_value(token);
}

std::shared_ptr<PeerJsSocket> PeerJsSocket::create(std::shared_ptr<BackendLoop> loop,
                                                   const std::string &url, const std::string &key,
                                                   std::string id, Handlers handlers) {
    if (!handlers.signal || !handlers.gone || !handlers.closed)
        throw std::invalid_argument("PeerJS socket requires handlers");
    auto socket =
        std::shared_ptr<PeerJsSocket>(new PeerJsSocket(loop, std::move(id), std::move(handlers)));
    const std::weak_ptr<PeerJsSocket> weak = socket;
    socket->channel_ = SocketChannel::create(
        std::move(loop), peerjs_socket_url(url, key, socket->id_, random_id()), {},
        {[weak](SocketMessage data) {
             if (auto self = weak.lock())
                 self->receive(std::move(data));
         },
         [weak] {
             if (auto self = weak.lock())
                 self->lost();
         }});
    return socket;
}

PeerJsSocket::PeerJsSocket(std::shared_ptr<BackendLoop> loop, std::string id, Handlers handlers)
    : loop_(std::move(loop)), id_(std::move(id)), handlers_(std::move(handlers)) {}

PeerJsSocket::~PeerJsSocket() {
    loop_->cancel(timer_);
    loop_->cancel(heartbeat_);
}

void PeerJsSocket::register_id(Registered done) {
    if (registration_) {
        done(*registration_);
        return;
    }
    waiting_.push_back(std::move(done));
    if (waiting_.size() > 1)
        return;
    timer_ = loop_->post_after(backend_timeout, [weak = weak_from_this()] {
        if (auto self = weak.lock())
            self->finish(BackendFailure::Transient);
    });
    channel_->ready([weak = weak_from_this()](std::optional<BackendFailure> failure) {
        auto self = weak.lock();
        if (self && failure)
            self->finish(*failure);
    });
}

void PeerJsSocket::finish(std::variant<Registration, BackendFailure> result) {
    loop_->cancel(timer_);
    timer_ = 0;
    if (const auto *state = std::get_if<Registration>(&result))
        registration_ = *state;
    for (auto &done : std::exchange(waiting_, {}))
        done(result);
}

void PeerJsSocket::receive(SocketMessage data) {
    const auto *text = std::get_if<std::string>(&data);
    const auto message = nlohmann::json::parse(
        text ? std::string_view(*text)
             : std::string_view(reinterpret_cast<const char *>(std::get<Bytes>(data).data()),
                                std::get<Bytes>(data).size()),
        nullptr, false);
    if (message.is_object() && message.contains("type") && message["type"].is_string())
        received(message);
}

void PeerJsSocket::received(const nlohmann::json &message) {
    const auto &type = message["type"].get_ref<const std::string &>();
    const bool from = message.contains("src") && message["src"].is_string();
    if (type == "OPEN") {
        beat();
        finish(Registration::Open);
    } else if (type == "ID-TAKEN") {
        finish(Registration::Taken);
        channel_->close();
    } else if (type == "ERROR") {
        std::string text;
        if (const auto payload = message.find("payload");
            payload != message.end() && payload->is_object() && payload->contains("msg") &&
            (*payload)["msg"].is_string())
            text = (*payload)["msg"].get<std::string>();
        const bool busy = std::regex_search(text, std::regex("limit", std::regex::icase));
        finish(busy ? BackendFailure::Transient : BackendFailure::Rejected);
    } else if (signal_type(type) && from) {
        handlers_.signal(message);
    } else if ((type == "EXPIRE" || type == "LEAVE") && from) {
        handlers_.gone(message["src"].get<std::string>());
    }
}

void PeerJsSocket::beat() {
    loop_->cancel(heartbeat_);
    heartbeat_ = loop_->post_after(heartbeat_interval, [weak = weak_from_this()] {
        auto self = weak.lock();
        if (!self || self->registration_ != Registration::Open)
            return;
        self->channel_->send(std::string(R"({"type":"HEARTBEAT"})"));
        self->beat();
    });
}

bool PeerJsSocket::send(const std::string &type, const std::string &dst, nlohmann::json payload) {
    if (!signal_type(type))
        throw std::invalid_argument("PeerJS servers relay only WebRTC signalling");
    return channel_->send(
        nlohmann::json{{"type", type}, {"payload", std::move(payload)}, {"dst", dst}}.dump());
}

// The next register_id() connects again, under the same ID and token.
void PeerJsSocket::lost() {
    loop_->cancel(heartbeat_);
    registration_.reset();
    finish(BackendFailure::Transient);
    handlers_.closed(BackendFailure::Transient);
}

void PeerJsSocket::close() {
    loop_->cancel(heartbeat_);
    registration_.reset();
    channel_->close();
    finish(BackendFailure::Transient);
}

} // namespace p2pdc::detail
