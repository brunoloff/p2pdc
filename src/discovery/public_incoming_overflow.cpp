#include "../security/signaling_limits.hpp"
#include "../core/wall_clock.hpp"
#include "public_incoming_impl.hpp"
#include "terminal_signal.hpp"

#include <iostream>
#include <limits>

namespace p2pdc::detail {

void PublicIncoming::reject_offer(const IncomingOffer &offer) noexcept {
    try {
        const auto now = forward_utc_seconds();
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
