#include "native_signaling.hpp"

#include "datagram_support.hpp"

#include <string>
#include <utility>

namespace p2pdc::detail {

NativeSignaling::NativeSignaling(NativeSignalBinding binding, ByteView capability,
                                 NativeReliableLink &link, CandidateLimits limits,
                                 MemoryBudget budget, std::size_t max_candidate_bytes,
                                 std::size_t max_candidates, std::function<bool(Bytes)> submit,
                                 std::function<bool(SignalingKind)> control, bool datagrams)
    : binding_(binding), link_(link), limits_(limits),
      session_(binding.local, binding.remote, binding.local_box, binding.remote_box,
               binding.attempt, binding.role, capability, 63),
      flow_(session_, limits, std::move(budget), max_candidate_bytes, max_candidates,
            {std::move(submit), [this](ByteView value) { return apply_description(value); },
             [this](CandidateView value) { return apply_candidate(value); }, std::move(control)}),
      datagrams_(datagrams) {}

FlowResult NativeSignaling::state() const {
    return closed_ ? FlowResult::Closed : failed_ ? FlowResult::Failed : FlowResult::Accepted;
}

FlowResult NativeSignaling::local_description(const Identity &identity, ByteView capability,
                                              rtc::Description value, std::uint64_t issued,
                                              std::uint64_t expiry, SignalingPolicy policy) {
    if (state() != FlowResult::Accepted) {
        return state();
    }
    const auto expected = binding_.role == HandshakeRole::Offerer ? rtc::Description::Type::Offer
                                                                  : rtc::Description::Type::Answer;
    if (value.type() != expected) {
        failed_ = true;
        return FlowResult::Failed;
    }
    try {
        const auto attribute = binding_.role == HandshakeRole::Offerer ? datagram_offer_attribute
                                                                       : datagram_answer_attribute;
        const auto text =
            datagrams_ ? advertise_datagrams(std::string(value), attribute) : std::string(value);
        const auto kind =
            binding_.role == HandshakeRole::Offerer ? SignalingKind::Offer : SignalingKind::Answer;
        auto result = flow_.send(identity, binding_.remote_box, capability, kind,
                                 std::as_bytes(std::span(text)), issued, expiry, policy);
        failed_ = result == FlowResult::Failed;
        return result;
    } catch (const std::exception &) {
        failed_ = true;
        return FlowResult::Failed;
    }
}

FlowResult NativeSignaling::local_candidate(const Identity &identity, ByteView capability,
                                            rtc::Candidate value, std::uint64_t issued,
                                            std::uint64_t expiry, SignalingPolicy policy) {
    if (state() != FlowResult::Accepted) {
        return state();
    }
    try {
        const auto candidate = value.candidate();
        const auto mid = value.mid();
        auto payload = encode_candidate({candidate, mid}, limits_);
        if (!std::holds_alternative<Bytes>(payload)) {
            failed_ = true;
            return FlowResult::Failed;
        }
        auto result =
            flow_.send(identity, binding_.remote_box, capability, SignalingKind::Candidate,
                       std::get<Bytes>(payload), issued, expiry, policy);
        failed_ = result == FlowResult::Failed;
        return result;
    } catch (const std::exception &) {
        failed_ = true;
        return FlowResult::Failed;
    }
}

FlowResult NativeSignaling::receive(ByteView wire, const RecipientKeyPair &local_box,
                                    ByteView capability, SignalingPolicy policy) {
    if (state() != FlowResult::Accepted) {
        return state();
    }
    auto result = flow_.receive(wire, local_box, capability, policy);
    failed_ = result == FlowResult::Failed;
    return result;
}

bool NativeSignaling::apply_description(ByteView payload) {
    try {
        const std::string text(reinterpret_cast<const char *>(payload.data()), payload.size());
        const auto type = binding_.role == HandshakeRole::Offerer ? rtc::Description::Type::Answer
                                                                  : rtc::Description::Type::Offer;
        link_.set_remote_description(rtc::Description(text, type));
        const auto attribute = binding_.role == HandshakeRole::Offerer ? datagram_answer_attribute
                                                                       : datagram_offer_attribute;
        if (datagrams_ && advertises_datagrams(text, attribute))
            link_.enable_datagrams();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool NativeSignaling::apply_candidate(CandidateView value) {
    try {
        link_.add_remote_candidate(
            rtc::Candidate(std::string(value.candidate), std::string(value.mid)));
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

std::optional<HandshakeContext> NativeSignaling::handshake_context() const {
    if (state() != FlowResult::Accepted) {
        return std::nullopt;
    }
    auto context = session_.handshake_context();
    if (auto *value = std::get_if<HandshakeContext>(&context)) {
        return *value;
    }
    return std::nullopt;
}

void NativeSignaling::close() {
    closed_ = true;
    flow_.close();
}

} // namespace p2pdc::detail
