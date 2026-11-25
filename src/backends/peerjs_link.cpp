#include "peerjs_link.hpp"

#include <nlohmann/json.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/peerconnection.hpp>

#include <iostream>

namespace p2pdc::detail {
namespace {
// Every browser takes data-channel messages of 64 KiB; replies to lookups are longer.
constexpr std::size_t fragment_chars = 16384;
constexpr std::size_t max_message_chars = 2 * 1024 * 1024;
constexpr std::size_t max_buffered_bytes = 8 * 1024 * 1024;
} // namespace

std::shared_ptr<PeerJsLink> PeerJsLink::create(std::shared_ptr<BackendLoop> loop, std::string peer,
                                               std::shared_ptr<rtc::PeerConnection> connection,
                                               std::shared_ptr<rtc::DataChannel> channel) {
    auto link = std::shared_ptr<PeerJsLink>(new PeerJsLink(
        std::move(loop), std::move(peer), std::move(connection), std::move(channel)));
    link->attach();
    return link;
}

PeerJsLink::PeerJsLink(std::shared_ptr<BackendLoop> loop, std::string peer,
                       std::shared_ptr<rtc::PeerConnection> connection,
                       std::shared_ptr<rtc::DataChannel> channel)
    : loop_(std::move(loop)), peer_(std::move(peer)), connection_(std::move(connection)),
      channel_(std::move(channel)) {}

PeerJsLink::~PeerJsLink() {
    if (!closed_) {
        channel_->resetCallbacks();
        connection_->resetCallbacks();
        channel_->close();
        connection_->close();
    }
}

void PeerJsLink::attach() {
    const std::weak_ptr<PeerJsLink> weak = weak_from_this();
    const auto loop = loop_;
    const auto closing = [loop, weak] {
        loop->post([weak] {
            if (auto self = weak.lock())
                self->close();
        });
    };
    channel_->onMessage([loop, weak](rtc::message_variant data) {
        loop->post([weak, data = std::move(data)]() mutable {
            auto self = weak.lock();
            if (!self)
                return;
            if (auto *text = std::get_if<std::string>(&data))
                self->receive(std::move(*text));
            else
                self->close(); // the record protocol is text
        });
    });
    channel_->onClosed(closing);
    channel_->onError([closing](const std::string &) { closing(); });
    connection_->onStateChange([closing](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed)
            closing();
    });
}

bool PeerJsLink::send(const nlohmann::json &message) {
    if (closed_ || !channel_->isOpen())
        return false;
    const auto text = message.dump();
    if (text.size() > max_message_chars ||
        channel_->bufferedAmount() + text.size() > max_buffered_bytes) {
        close();
        return false;
    }
    try {
        for (std::size_t start = 0; start < text.size() || start == 0; start += fragment_chars) {
            const bool more = start + fragment_chars < text.size();
            channel_->send((more ? "+" : "=") + text.substr(start, fragment_chars));
        }
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: PeerJS data connection send failed: " << error.what() << '\n';
        close();
        return false;
    }
    return true;
}

void PeerJsLink::receive(std::string fragment) {
    if (closed_ || fragment.empty() || (fragment[0] != '+' && fragment[0] != '=')) {
        close();
        return;
    }
    partial_.append(fragment, 1);
    if (partial_.size() > max_message_chars) {
        close();
        return;
    }
    if (fragment[0] == '+')
        return;
    const auto message = nlohmann::json::parse(std::exchange(partial_, {}), nullptr, false);
    if (message.is_discarded()) {
        close();
        return;
    }
    if (message.is_object())
        on_message(message);
}

void PeerJsLink::close() {
    if (std::exchange(closed_, true))
        return;
    channel_->resetCallbacks();
    connection_->resetCallbacks();
    try {
        channel_->close();
        connection_->close();
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: closing a PeerJS data connection failed: " << error.what() << '\n';
    }
    auto self = shared_from_this(); // the handlers may drop the last owner
    released();
    on_close();
}

} // namespace p2pdc::detail
