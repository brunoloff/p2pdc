#pragma once

#include "../discovery/connection_admission.hpp"
#include "../discovery/incoming_offer_resolver.hpp"
#include "../security/async_signaling.hpp"

#include <stdexcept>

namespace p2pdc::detail {
class IncomingOfferInvalid : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};
class IncomingCryptoFailure : public std::runtime_error {
  public:
    explicit IncomingCryptoFailure(CryptoError error);
    [[nodiscard]] CryptoError error() const noexcept { return error_; }

  private:
    CryptoError error_;
};

[[nodiscard]] AsyncSignalingBinding make_attempt_binding(const ConnectionAdmission &admission,
                                                         const MemoryBudget &budget,
                                                         const std::function<std::uint64_t()> &now,
                                                         AttemptId attempt, HandshakeRole role);
[[nodiscard]] AttemptId random_attempt_id();
void verify_incoming_offer(const ConnectionAdmission &admission, const IncomingOffer &offer,
                           const AsyncSignalingBinding &binding, SignalingPolicy policy);
} // namespace p2pdc::detail
