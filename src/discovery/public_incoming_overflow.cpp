#include "../security/signaling_limits.hpp"
#include "public_incoming_impl.hpp"
#include "terminal_signal.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace p2pdc::detail {
static std::uint64_t utc_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("system clock is before the Unix epoch");
    return static_cast<std::uint64_t>(value);
}

void PublicIncoming::reject_offer(const IncomingOffer &offer) noexcept {
    try {
        const auto now = utc_seconds();
        if (now > std::numeric_limits<std::uint64_t>::max() - 50)
            throw std::overflow_error("rejection expiry overflow");
        const auto &remote = offer.peer;
        SignalingFields fields{SignalingKind::Reject,
                               initial_.local.endpoint.fields.instance_id,
                               {remote.signing_public_key, remote.fields.instance_id},
                               offer.envelope.fields.attempt_id,
                               0,
                               now,
                               now + 50,
                               {}};
        (void)publish_terminal_signal(
            store_, remote.fields.signaling_inbox, *initial_.local.endpoint.identity->signing,
            std::move(fields), remote.fields.recipient_public_key, *initial_.local.capability,
            {now, 60, initial_.remote.clock_skew, signaling_payload_limit});
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: " << error.what() << '\n';
    }
}
void PublicIncoming::report_overflow() {
    if (!handlers_.on_overflow || overflow_queued_)
        return;
    overflow_queued_ = true;
    if (!gate_.post([self = shared_from_this()] {
            self->overflow_queued_ = false;
            if (self->state_ != IncomingState::Listening)
                return;
            try {
                self->handlers_.on_overflow();
            } catch (...) {
                self->fail(IncomingError::Callback);
                throw;
            }
        }))
        fail(IncomingError::Backend);
}
} // namespace p2pdc::detail
