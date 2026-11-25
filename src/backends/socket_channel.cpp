#include "socket_channel.hpp"

#include <rtc/websocket.hpp>

#include <filesystem>
#include <iostream>
#include <utility>

namespace p2pdc::detail {
namespace {
constexpr std::chrono::milliseconds first_retry{1000}, retry_cap{30000};
// The largest message a backend accepts: four of the largest values with room for framing.
constexpr std::size_t max_message_bytes = 4 * 32768 + 65536;

// Where TLS finds the certificate authorities. OpenSSL looks where it was configured to, which
// for the OpenSSL that deps/ builds on macOS holds none; macOS keeps its bundle here.
std::optional<std::string> certificate_authorities() {
#ifdef __APPLE__
    if (std::filesystem::exists("/etc/ssl/cert.pem"))
        return "/etc/ssl/cert.pem";
#endif
    return std::nullopt;
}
} // namespace

std::shared_ptr<SocketChannel> SocketChannel::create(std::shared_ptr<BackendLoop> loop,
                                                     std::string url,
                                                     std::vector<std::string> protocols,
                                                     Handlers handlers) {
    if (!loop || !handlers.message || !handlers.closed)
        throw std::invalid_argument("socket channel requires a loop and handlers");
    return std::shared_ptr<SocketChannel>(new SocketChannel(
        std::move(loop), std::move(url), std::move(protocols), std::move(handlers)));
}

SocketChannel::SocketChannel(std::shared_ptr<BackendLoop> loop, std::string url,
                             std::vector<std::string> protocols, Handlers handlers)
    : loop_(std::move(loop)), url_(std::move(url)), protocols_(std::move(protocols)),
      handlers_(std::move(handlers)), retry_(first_retry) {}

SocketChannel::~SocketChannel() { release_socket(); }

bool SocketChannel::open() const { return is_open_ && socket_ && socket_->isOpen(); }

void SocketChannel::ready(Ready done) {
    if (closed_ || (!socket_ && std::chrono::steady_clock::now() < not_before_)) {
        done(BackendFailure::Transient);
        return;
    }
    if (open()) {
        done(std::nullopt);
        return;
    }
    waiting_.push_back(std::move(done));
    if (!socket_)
        connect();
}

void SocketChannel::connect() {
    rtc::WebSocket::Configuration config;
    config.protocols = protocols_;
    config.connectionTimeout = backend_timeout;
    config.maxMessageSize = max_message_bytes;
    config.caCertificatePemFile = certificate_authorities();
    const auto attempt = ++attempt_;
    const std::weak_ptr<SocketChannel> weak = weak_from_this();
    const auto loop = loop_;
    auto post = [loop, weak, attempt](auto step) {
        loop->post([weak, attempt, step] {
            if (auto self = weak.lock())
                step(*self, attempt);
        });
    };
    socket_ = std::make_shared<rtc::WebSocket>(config);
    socket_->onOpen([post] { post([](SocketChannel &self, auto id) { self.opened(id); }); });
    socket_->onError([post](const std::string &) {
        post([](SocketChannel &self, auto id) { self.failed(id); });
    });
    socket_->onClosed([post] { post([](SocketChannel &self, auto id) { self.failed(id); }); });
    socket_->onMessage([loop, weak, attempt](rtc::message_variant data) {
        loop->post([weak, attempt, data = std::move(data)]() mutable {
            auto self = weak.lock();
            if (!self || attempt != self->attempt_ || !self->is_open_)
                return;
            if (auto *text = std::get_if<std::string>(&data))
                self->handlers_.message(std::move(*text));
            else
                self->handlers_.message(std::get<rtc::binary>(std::move(data)));
        });
    });
    timer_ = loop_->post_after(backend_timeout, [weak, attempt] {
        if (auto self = weak.lock())
            self->failed(attempt);
    });
    try {
        socket_->open(url_);
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: cannot open " << url_ << ": " << error.what() << '\n';
        failed(attempt);
    }
}

void SocketChannel::opened(std::uint64_t attempt) {
    if (attempt != attempt_ || is_open_ || !socket_)
        return;
    loop_->cancel(timer_);
    is_open_ = true;
    retry_ = first_retry;
    finish(std::nullopt);
}

// A failed attempt backs off; an open socket that closes tells the owner.
void SocketChannel::failed(std::uint64_t attempt) {
    if (attempt != attempt_ || !socket_)
        return;
    const bool was_open = is_open_;
    release_socket();
    if (!was_open) {
        not_before_ = std::chrono::steady_clock::now() + retry_;
        retry_ = std::min(retry_ * 2, retry_cap);
        finish(BackendFailure::Transient);
        return;
    }
    handlers_.closed();
}

void SocketChannel::finish(std::optional<BackendFailure> result) {
    for (auto &done : std::exchange(waiting_, {}))
        done(result);
}

void SocketChannel::release_socket() {
    loop_->cancel(timer_);
    ++attempt_;
    is_open_ = false;
    if (auto socket = std::exchange(socket_, nullptr)) {
        socket->resetCallbacks();
        try {
            socket->close();
        } catch (const std::exception &error) {
            std::cerr << "p2pdc: closing " << url_ << " failed: " << error.what() << '\n';
        }
    }
}

bool SocketChannel::send(std::string text) {
    if (!open())
        return false;
    try {
        return socket_->send(std::move(text));
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: sending to " << url_ << " failed: " << error.what() << '\n';
        return false;
    }
}

bool SocketChannel::send(ByteView bytes) {
    if (!open())
        return false;
    try {
        return socket_->send(bytes.data(), bytes.size());
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: sending to " << url_ << " failed: " << error.what() << '\n';
        return false;
    }
}

void SocketChannel::drop() {
    release_socket();
    not_before_ = std::chrono::steady_clock::now() + retry_;
    retry_ = std::min(retry_ * 2, retry_cap);
    finish(BackendFailure::Transient);
}

void SocketChannel::close() {
    closed_ = true;
    release_socket();
    finish(BackendFailure::Transient);
}

} // namespace p2pdc::detail
