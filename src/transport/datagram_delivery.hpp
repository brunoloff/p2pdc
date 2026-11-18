#pragma once

#include "../core/deferred_events.hpp"
#include "p2pdc/frame.h"

#include <functional>

namespace p2pdc::detail {

// Hands received datagrams to the application through a small queue of their own, so
// they can never use up the room reliable messages need. When it is full they are
// dropped, which datagram receivers must tolerate anyway.
class DatagramDelivery {
  public:
    explicit DatagramDelivery(Executor executor);
    void post(Bytes value, std::function<void(Bytes)> deliver) noexcept;
    void close();

  private:
    DeferredEvents events_;
};

} // namespace p2pdc::detail
