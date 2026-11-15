#include "reliable_pump.hpp"

#include <stdexcept>

namespace p2pdc::detail {

ReliablePump::ReliablePump(ReliableSender &sender, ReliableSink &sink, std::size_t high_water,
                           std::size_t max_frames_per_pass)
    : sender_(sender), sink_(sink), high_water_(high_water),
      max_frames_per_pass_(max_frames_per_pass) {
    if (high_water < frame_header_size || max_frames_per_pass == 0) {
        throw std::invalid_argument("invalid reliable pump limits");
    }
}

PumpResult ReliablePump::fail() {
    failed_ = true;
    sender_.clear();
    return PumpResult::Failed;
}

PumpResult ReliablePump::run() {
    if (failed_) {
        return PumpResult::Failed;
    }
    for (std::size_t count = 0; count < max_frames_per_pass_; ++count) {
        auto pending = sender_.peek();
        if (pending.error) {
            return *pending.error == SenderFrameError::Backpressure ? PumpResult::Blocked : fail();
        }
        if (!pending.frame) {
            return PumpResult::Idle;
        }
        const auto size = pending.frame->size();
        if (size > high_water_) {
            return fail();
        }
        const auto buffered = sink_.buffered_amount();
        if (buffered > high_water_ - size) {
            return PumpResult::Blocked;
        }
        const auto sent = sink_.send(*pending.frame);
        if (sent == SinkResult::NotReady) {
            return PumpResult::Blocked;
        }
        if (sent != SinkResult::Accepted) {
            return fail();
        }
        sender_.commit();
    }
    return sender_.queued_messages() == 0 ? PumpResult::Idle : PumpResult::More;
}

} // namespace p2pdc::detail
