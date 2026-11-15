#include "native_incoming_config.hpp"
#include "native_outgoing_config.hpp"
#include "public_attempt.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
std::uint64_t now_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("system clock is before the Unix epoch");
    return static_cast<std::uint64_t>(value);
}
} // namespace

std::variant<std::unique_ptr<PublicAttempt>, StoreError>
make_public_attempt(const ConnectionAdmission &admission, const IncomingOffer *offer, IceConfig ice,
                    MemoryBudget budget, std::uint32_t message_limit, Executor executor,
                    ConnectionAttemptCallbacks callbacks) {
    auto config =
        offer ? make_native_incoming_config(admission, *offer, std::move(ice), budget, now_seconds)
              : make_native_outgoing_config(admission, std::move(ice), budget, now_seconds);
    config.application_limit = message_limit;
    config.gate_transport_limit =
        std::max(config.gate_transport_limit, static_cast<std::size_t>(message_limit) + 1);
    config.channel_limits.frames.max_message_size =
        static_cast<std::uint32_t>(config.gate_transport_limit);
    config.channel_limits.send_bytes =
        std::max(config.channel_limits.send_bytes, config.gate_transport_limit);
    auto created =
        NativeAttempt::create(std::move(config), std::move(executor), std::move(callbacks));
    if (auto *error = std::get_if<StoreError>(&created))
        return *error;
    return std::unique_ptr<PublicAttempt>(
        std::move(std::get<std::unique_ptr<NativeAttempt>>(created)));
}
} // namespace p2pdc::detail
