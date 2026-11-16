#include "../security/signaling_limits.hpp"
#include "../core/callback_failure.hpp"
#include "../core/wall_clock.hpp"
#include "public_incoming_impl.hpp"
#include "terminal_signal.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace p2pdc::detail {

void PublicIncoming::reject_offer(const IncomingOffer &offer) noexcept {
    send_reject(offer, initial_.local.endpoint, initial_.local.capability);
}
// Uses the given endpoint and capability rather than the listener's own, which
// are released once a single-shot request is taken.
void PublicIncoming::send_reject(const IncomingOffer &offer, const AnnouncementEndpoint &local,
                                 const std::shared_ptr<const Bytes> &capability) noexcept {
    try {
        if (!local.identity || !local.identity->signing || !capability)
            throw std::logic_error("rejection needs the local identity and capability");
        const auto now = forward_utc_seconds();
        if (now > std::numeric_limits<std::uint64_t>::max() - 50)
            throw std::overflow_error("rejection expiry overflow");
        const auto &remote = offer.peer;
        SignalingFields fields{SignalingKind::Reject,
                               local.fields.instance_id,
                               {remote.signing_public_key, remote.fields.instance_id},
                               offer.envelope.fields.attempt_id,
                               0,
                               now,
                               now + 50,
                               {}};
        (void)publish_terminal_signal(
            store_, remote.fields.signaling_inbox, *local.identity->signing, std::move(fields),
            remote.fields.recipient_public_key, *capability,
            {now, 60, initial_.remote.clock_skew, signaling_payload_limit});
    } catch (const std::exception &error) {
        std::cerr << "p2pdc: cannot send rejection: " << error.what() << '\n';
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
                log_callback_exception("on_overflow");
            }
        }))
        fail(IncomingError::Backend);
}
} // namespace p2pdc::detail
