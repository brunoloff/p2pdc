#pragma once

#include "../transport/attempt_binding.hpp"
#include "browser_mailbox_attempt.hpp"

namespace p2pdc::detail {
[[nodiscard]] BrowserMailboxConfig make_browser_public_config(const ConnectionAdmission &admission,
                                                              const IncomingOffer *offer,
                                                              IceConfig ice, MemoryBudget budget,
                                                              std::uint32_t message_limit,
                                                              std::function<std::uint64_t()> now);
} // namespace p2pdc::detail
