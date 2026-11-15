#pragma once

#include "reliable_sender.hpp"

namespace p2pdc::detail {

enum class SinkResult { Accepted, NotReady, Failed };
enum class PumpResult { Idle, More, Blocked, Failed };

// All methods run in the same serialized context as the sender. Implementations
// copy or accept the entire frame before returning Accepted and never throw.
// Sink methods must not reenter the pump, mutate its sender, or destroy either.
class ReliableSink {
  public:
    virtual ~ReliableSink() = default;
    [[nodiscard]] virtual std::size_t buffered_amount() const = 0;
    [[nodiscard]] virtual SinkResult send(ByteView frame) = 0;
};

// The caller schedules another pass after More, and waits for a backend or
// memory-availability event after Blocked. Failure closes and clears the sender.
class ReliablePump {
  public:
    ReliablePump(ReliableSender &sender, ReliableSink &sink, std::size_t high_water,
                 std::size_t max_frames_per_pass);
    [[nodiscard]] PumpResult run();

  private:
    [[nodiscard]] PumpResult fail();
    ReliableSender &sender_;
    ReliableSink &sink_;
    std::size_t high_water_;
    std::size_t max_frames_per_pass_;
    bool failed_ = false;
};

} // namespace p2pdc::detail
