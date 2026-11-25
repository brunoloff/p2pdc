#include "native_outgoing_config.hpp"
#include "../security/signaling_limits.hpp"
#include "attempt_binding.hpp"

#include <utility>

namespace p2pdc::detail {
namespace {
constexpr std::size_t wire_limit = signaling_wire_limit;
constexpr std::size_t outbox_bytes = 524288;
constexpr std::uint32_t application_limit = 256 * 1024;
// Applications often run tasks one event-loop pass at a time, so each pass must move many
// frames: 64 frames of 1 KiB a pass, with room in the backend for a few passes.
constexpr std::size_t pass_frames = 64;
constexpr std::size_t backend_bytes = 256 * 1024;

} // namespace

NativeAttemptConfig make_native_outgoing_config(const ConnectionAdmission &admission, IceConfig ice,
                                                MemoryBudget budget,
                                                std::function<std::uint64_t()> now) {
    return make_native_attempt_config(admission, std::move(ice), std::move(budget), std::move(now),
                                      random_attempt_id(), HandshakeRole::Offerer);
}

NativeAttemptConfig make_native_attempt_config(const ConnectionAdmission &admission, IceConfig ice,
                                               MemoryBudget budget,
                                               std::function<std::uint64_t()> now,
                                               AttemptId attempt, HandshakeRole role) {
    const auto common = make_attempt_binding(admission, budget, now, attempt, role);
    const auto &local = admission.local;
    const auto &remote = admission.remote;
    NativeSignalBinding binding{common.local,      common.remote,  common.local_box,
                                common.remote_box, common.attempt, common.role};
    return {make_native_record_store(admission.runner, admission.backends,
                                     {wire_limit, 128, 1024 * 1024}),
            local.fields.signaling_inbox,
            remote.fields.signaling_inbox,
            binding,
            local.identity->signing,
            local.identity->recipient,
            admission.capability,
            std::move(ice),
            budget,
            {{outbox_bytes, 128, wire_limit}, {outbox_bytes, 64, wire_limit}},
            {{application_limit + 1, 1024}, 2 * 1024 * 1024, 32, backend_bytes, pass_frames},
            NativeReceiveLimits{1024},
            {4096, 256, 4361},
            65536,
            64,
            application_limit,
            application_limit + 1,
            {0, 60, admission.remote_clock_skew, signaling_payload_limit},
            50,
            std::move(now)};
}
} // namespace p2pdc::detail
