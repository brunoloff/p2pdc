#include "datagram_delivery.hpp"

#include "p2pdc/connection.h"

#include <exception>
#include <utility>

namespace p2pdc::detail {
namespace {
constexpr std::size_t max_pending_datagrams = 32;
constexpr std::size_t max_pending_bytes = max_pending_datagrams * max_datagram_bytes;
} // namespace

DatagramDelivery::DatagramDelivery(Executor executor)
    : events_(std::move(executor), MemoryBudget(max_pending_bytes), max_pending_datagrams,
              max_pending_bytes) {}

void DatagramDelivery::post(Bytes value, std::function<void(Bytes)> deliver) noexcept {
    try {
        const auto size = value.size();
        (void)events_.post(size,
                           [value = std::move(value), deliver = std::move(deliver)]() mutable {
                               deliver(std::move(value));
                           });
    } catch (const std::exception &) {
        // Losing a datagram is allowed; the queue closes itself after such a failure.
    }
}

void DatagramDelivery::close() { events_.close(); }

} // namespace p2pdc::detail
