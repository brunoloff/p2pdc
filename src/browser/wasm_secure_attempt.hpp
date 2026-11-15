#pragma once

#include "../security/secure_attempt.hpp"

#include <memory>

namespace p2pdc::detail {

// Bridge configuration is exactly 213 bytes: seed, box secret, capability,
// remote signing key, remote box key (32 bytes each), local instance, remote
// instance, attempt ID (16 bytes each), role (1=offerer, 2=answerer), and
// maximum application message size (4-byte big-endian unsigned integer).
inline constexpr std::size_t wasm_attempt_config_size = 213;

class WasmSecureAttempt final : public ReliableSink,
                                public std::enable_shared_from_this<WasmSecureAttempt> {
  public:
    static std::shared_ptr<WasmSecureAttempt> create(unsigned id, ByteView config);
    ~WasmSecureAttempt();
    [[nodiscard]] std::size_t buffered_amount() const override;
    [[nodiscard]] SinkResult send(ByteView frame) override;
    [[nodiscard]] SecureAttempt &attempt() { return *attempt_; }
    [[nodiscard]] bool active() const { return active_; }
    void fail(AttemptFailure reason);
    void close();

  private:
    WasmSecureAttempt(unsigned id, Identity identity, RecipientKeyPair box);
    void initialize(ByteView config);
    [[nodiscard]] SecureAttemptCallbacks callbacks();
    [[nodiscard]] SignalingFlowCallbacks signaling_callbacks();
    [[nodiscard]] Executor executor();
    void report(AttemptFailure reason);
    void defer_failure();
    unsigned id_;
    Identity identity_;
    RecipientKeyPair box_;
    std::unique_ptr<SecureAttempt> attempt_;
    bool active_ = true;
};

} // namespace p2pdc::detail
