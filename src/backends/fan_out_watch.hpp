#pragma once

#include "backend_loop.hpp"
#include "fan_out_store.hpp"

#include <functional>
#include <memory>
#include <variant>
#include <vector>

namespace p2pdc::detail {

// The fan-out store's watch (fan_out_store.hpp): ready once one store's watch is; one that
// fails is watched again after a backoff. Its failures reach the consumer only when every
// store failed before any became ready: then the consumer gets one error batch and the
// registration reports zero.
[[nodiscard]] std::variant<RecordWatch, StoreError>
start_fan_out_watch(std::shared_ptr<BackendLoop> loop, const std::vector<RecordStore> &stores,
                    ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate,
                    FanOutTiming timing);

} // namespace p2pdc::detail
