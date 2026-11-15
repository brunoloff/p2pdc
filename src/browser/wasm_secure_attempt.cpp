#include "wasm_secure_attempt.hpp"

#include <emscripten.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

using namespace p2pdc;
using namespace p2pdc::detail;

EM_JS(int, secure_bytes, (unsigned id, unsigned kind, const std::byte *data, unsigned size), {
    try {
        return Module.p2pdcSecureBridge.bytes(id, kind, HEAPU8.slice(data, data + size)) ? 1 : 0;
    } catch (error) {
        console.error("p2pdc: secure byte delivery failed");
        return 0;
    }
});
EM_JS(int, secure_candidate,
      (unsigned id, unsigned operation, const char *candidate, unsigned candidate_size,
       const char *mid, unsigned mid_size),
      {
          try {
              return Module.p2pdcSecureBridge.candidate(
                         id, operation, HEAPU8.slice(candidate, candidate + candidate_size),
                         HEAPU8.slice(mid, mid + mid_size))
                         ? 1
                         : 0;
          } catch (error) {
              console.error("p2pdc: candidate delivery failed");
              return 0;
          }
      });
EM_JS(int, secure_number, (unsigned id, unsigned kind, unsigned value), {
    try {
        return Module.p2pdcSecureBridge.number(id, kind, value) ? 1 : 0;
    } catch (error) {
        console.error("p2pdc: secure event delivery failed");
        return 0;
    }
});
EM_JS(int, secure_frame, (unsigned id, const std::byte *data, unsigned size), {
    try {
        return Module.p2pdcSecureBridge.frame(id, HEAPU8.slice(data, data + size));
    } catch (error) {
        console.error("p2pdc: secure frame submission failed");
        return 2;
    }
});
EM_JS(unsigned, secure_buffered, (unsigned id), {
    try {
        return Module.p2pdcSecureBridge.buffered(id);
    } catch (error) {
        console.error("p2pdc: secure buffer query failed");
        return 0xffffffff;
    }
});

namespace p2pdc::detail {
namespace {
template <std::size_t N> std::array<std::byte, N> array(ByteView input, std::size_t offset) {
    std::array<std::byte, N> out{};
    std::copy_n(input.begin() + offset, N, out.begin());
    return out;
}
std::uint32_t message_limit(ByteView config) {
    std::uint32_t limit = 0;
    for (std::size_t index = 209; index < 213; ++index)
        limit = (limit << 8U) | std::to_integer<std::uint32_t>(config[index]);
    if (!limit || limit > 4 * 1024 * 1024)
        throw std::invalid_argument("invalid application message limit");
    return limit;
}
void post(std::function<void()> callback) {
    auto task = std::make_unique<std::function<void()>>(std::move(callback));
    emscripten_async_call(
        [](void *data) {
            std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()> *>(data));
            (*task)();
        },
        task.release(), 0);
}
} // namespace

WasmSecureAttempt::WasmSecureAttempt(unsigned id, Identity identity, RecipientKeyPair box)
    : id_(id), identity_(std::move(identity)), box_(std::move(box)) {}
WasmSecureAttempt::~WasmSecureAttempt() { close(); }
std::shared_ptr<WasmSecureAttempt> WasmSecureAttempt::create(unsigned id, ByteView config) {
    if (!id || config.size() != wasm_attempt_config_size)
        throw std::invalid_argument("invalid config");
    (void)message_limit(config);
    auto identity = Identity::import_seed(config.subspan(0, 32));
    auto box = RecipientKeyPair::import_secret_key(config.subspan(32, 32));
    if (!std::holds_alternative<Identity>(identity) ||
        !std::holds_alternative<RecipientKeyPair>(box))
        throw std::invalid_argument("cannot import credentials");
    auto owner = std::shared_ptr<WasmSecureAttempt>(new WasmSecureAttempt(
        id, std::move(std::get<Identity>(identity)), std::move(std::get<RecipientKeyPair>(box))));
    owner->initialize(config);
    return owner;
}
void WasmSecureAttempt::initialize(ByteView config) {
    const auto role = std::to_integer<unsigned>(config[208]);
    if (role != 1 && role != 2)
        throw std::invalid_argument("invalid role");
    AsyncSignalingBinding binding{{identity_.public_key(), array<16>(config, 160)},
                                  {array<32>(config, 96), array<16>(config, 176)},
                                  box_.public_key(),
                                  array<32>(config, 128),
                                  array<16>(config, 192),
                                  static_cast<HandshakeRole>(role)};
    const auto receive_limit = message_limit(config);
    const auto frame_limit = std::max<std::uint32_t>(262145, receive_limit + 1);
    const ReliableChannelLimits limits{
        {frame_limit, 1024}, std::max<std::size_t>(2097152, frame_limit), 32, 524288, 4};
    attempt_ = std::make_unique<SecureAttempt>(
        binding, config.subspan(64, 32), identity_, box_, CandidateLimits{4096, 256, 4361},
        MemoryBudget(std::max<std::size_t>(4194304, 2 * receive_limit + 1048576)), limits,
        receive_limit, *this, executor(), callbacks());
}
Executor WasmSecureAttempt::executor() {
    auto weak = weak_from_this();
    return [weak](std::function<void()> task) {
        try {
            post([weak, task = std::move(task)] {
                try {
                    task();
                } catch (...) {
                    if (auto self = weak.lock())
                        self->fail(AttemptFailure::Resource);
                }
            });
        } catch (...) {
            if (auto self = weak.lock())
                self->fail(AttemptFailure::Resource);
        }
    };
}
SignalingFlowCallbacks WasmSecureAttempt::signaling_callbacks() {
    auto weak = weak_from_this();
    return {
        [weak](Bytes value) {
            auto self = weak.lock();
            return self && self->active_ && secure_bytes(self->id_, 0, value.data(), value.size());
        },
        [weak](ByteView value) {
            auto self = weak.lock();
            return self && self->active_ && secure_bytes(self->id_, 1, value.data(), value.size());
        },
        [weak](CandidateView value) {
            auto self = weak.lock();
            if (!self || !self->active_)
                return false;
            auto operation = self->attempt_->candidate_operation();
            if (!operation || *operation > 63)
                return false;
            return secure_candidate(self->id_, static_cast<unsigned>(*operation),
                                    value.candidate.data(), value.candidate.size(),
                                    value.mid.data(), value.mid.size()) != 0;
        },
        [weak](SignalingKind kind) {
            auto self = weak.lock();
            if (!self || !self->active_)
                return false;
            return secure_number(self->id_, 3, static_cast<unsigned>(kind)) != 0;
        }};
}
SecureAttemptCallbacks WasmSecureAttempt::callbacks() {
    auto weak = weak_from_this();
    return {
        signaling_callbacks(),
        [weak](std::uint32_t value) {
            if (auto self = weak.lock();
                self && self->active_ && !secure_number(self->id_, 0, value))
                self->defer_failure();
        },
        [weak](Bytes value) {
            if (auto self = weak.lock();
                self && self->active_ && !secure_bytes(self->id_, 2, value.data(), value.size())) {
                self->defer_failure();
            }
        },
        [weak] {
            if (auto self = weak.lock(); self && self->active_ && !secure_number(self->id_, 1, 0))
                self->defer_failure();
        },
        [weak](AttemptFailure reason) {
            if (auto self = weak.lock())
                self->report(reason);
        }};
}
void WasmSecureAttempt::defer_failure() {
    auto weak = weak_from_this();
    try {
        post([weak] {
            if (auto self = weak.lock())
                self->fail(AttemptFailure::Resource);
        });
    } catch (...) {
        report(AttemptFailure::Resource);
    }
}
std::size_t WasmSecureAttempt::buffered_amount() const { return secure_buffered(id_); }
SinkResult WasmSecureAttempt::send(ByteView frame) {
    if (!active_)
        return SinkResult::Failed;
    switch (secure_frame(id_, frame.data(), frame.size())) {
    case 0:
        return SinkResult::Accepted;
    case 1:
        return SinkResult::NotReady;
    default:
        return SinkResult::Failed;
    }
}
void WasmSecureAttempt::report(AttemptFailure reason) {
    if (!active_)
        return;
    active_ = false;
    secure_number(id_, 2, static_cast<unsigned>(reason));
}
void WasmSecureAttempt::fail(AttemptFailure reason) {
    if (!active_)
        return;
    report(reason);
    if (attempt_)
        attempt_->close();
}
void WasmSecureAttempt::close() {
    active_ = false;
    if (attempt_)
        attempt_->close();
}
} // namespace p2pdc::detail
