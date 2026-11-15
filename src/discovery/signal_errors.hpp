#pragma once

#include "native_signal_pipe.hpp"

namespace p2pdc::detail {
inline bool transient_signal_error(const OutboxError &error) {
    if (auto failure = std::get_if<OutboxFailure>(&error))
        return *failure == OutboxFailure::BackendFailed;
    if (auto store = std::get_if<StoreError>(&error))
        return *store == StoreError::NotRunning;
    return false;
}
inline bool resource_signal_error(std::optional<StoreError> error) {
    return error == StoreError::ResourceLimit || error == StoreError::AllocationFailed;
}
inline bool resource_signal_error(const OutboxError &error) {
    if (auto publish = std::get_if<PublishError>(&error))
        return *publish == PublishError::ResourceLimit;
    if (auto store = std::get_if<StoreError>(&error))
        return resource_signal_error(std::optional{*store});
    return false;
}
inline bool resource_signal_error(MailboxError error, std::optional<StoreError> detail) {
    return error == MailboxError::QueueFull || error == MailboxError::AllocationFailed ||
           error == MailboxError::RecordTooLarge || resource_signal_error(detail);
}
inline bool resource_catchup_error(const CatchupResult &error) {
    if (auto store = std::get_if<StoreError>(&error))
        return resource_signal_error(std::optional{*store});
    return false;
}
} // namespace p2pdc::detail
