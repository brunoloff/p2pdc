#include "native_mailbox.hpp"
#include "../core/submission_gate.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

struct NativeMailbox::Impl : std::enable_shared_from_this<Impl> {
    RecordStore store;
    std::array<std::byte, 20> key;
    InboundQueue inbox;
    SubmissionGate submissions;
    MailboxCallbacks callbacks;
    LookupDeduplication deduplication;
    std::optional<RecordWatch> watch;
    std::shared_future<std::size_t> registered;
    std::mutex submit_mutex;
    std::optional<StoreError> store_error;
    std::atomic_bool active{true};
    bool catching_up = false;

    Impl(RecordStore source, std::array<std::byte, 20> address, MailboxLimits limits,
         MemoryBudget budget, Executor dispatch, MailboxCallbacks handlers,
         LookupDeduplication deduplication_policy)
        : store(std::move(source)), key(address),
          inbox(std::move(budget), limits.queued_bytes, limits.queued_records, limits.record_bytes,
                InboundOverflow::Drop),
          submissions(std::move(dispatch)), callbacks(std::move(handlers)),
          deduplication(deduplication_policy) {}

    void schedule() {
        (void)submissions.post([weak = weak_from_this()] {
            if (auto self = weak.lock(); self && self->active) {
                self->drain();
            }
        });
    }

    void submit_error(StoreError error) {
        std::lock_guard lock(submit_mutex);
        if (!active || store_error) {
            return;
        }
        const auto pushed = inbox.reject_invalid_type();
        if (pushed.status == InboundStatus::InvalidType) {
            store_error = error;
        }
        if (pushed.schedule_drain) {
            schedule();
        }
    }

    void ingest(StoreBatch batch) noexcept {
        try {
            if (batch.error) {
                submit_error(*batch.error);
                return;
            }
            if (batch.expired) {
                return;
            }
            for (auto &value : batch.values) {
                std::lock_guard lock(submit_mutex);
                if (!active || store_error) {
                    return;
                }
                const auto pushed =
                    inbox.push(std::move(value), deduplication == LookupDeduplication::Content);
                if (pushed.schedule_drain) {
                    schedule();
                }
                if (pushed.status != InboundStatus::Accepted &&
                    pushed.status != InboundStatus::Dropped) {
                    return;
                }
            }
        } catch (const std::exception &error) {
            active = false;
            inbox.close();
            std::cerr << "NativeMailbox: cannot submit backend event: " << error.what() << '\n';
        }
    }

    void stop() {
        active = false;
        submissions.close();
        inbox.close();
        watch.reset();
    }

    void report(InboundStatus status) {
        std::optional<StoreError> detail;
        {
            std::lock_guard lock(submit_mutex);
            detail = store_error;
        }
        const auto error = status == InboundStatus::InvalidType ? MailboxError::StoreFailure
                           : status == InboundStatus::TooLarge  ? MailboxError::RecordTooLarge
                           : status == InboundStatus::AllocationFailed
                               ? MailboxError::AllocationFailed
                               : MailboxError::QueueFull;
        stop();
        callbacks.on_error(error, detail);
    }

    void drain() {
        for (unsigned count = 0; count < 16; ++count) {
            auto next = inbox.next();
            if (auto *message = std::get_if<InboundMessage>(&next)) {
                callbacks.on_record(std::move(message->payload));
                if (!active) {
                    return;
                }
            } else {
                const auto status = std::get<InboundStatus>(next);
                if (status != InboundStatus::Idle && status != InboundStatus::Closed) {
                    report(status);
                }
                return;
            }
        }
        schedule();
    }

    void catchup_complete(LookupResult result) noexcept {
        const bool success = result.backend_success;
        const bool invalid = result.batch.error.has_value();
        ingest(std::move(result.batch));
        if (invalid)
            return;
        try {
            (void)submissions.post([weak = weak_from_this(), success] {
                if (auto self = weak.lock(); self && self->active) {
                    self->callbacks.on_catchup(success);
                }
            });
        } catch (const std::exception &error) {
            active = false;
            inbox.close();
            std::cerr << "NativeMailbox: cannot submit catch-up result: " << error.what() << '\n';
        }
    }
};

NativeMailbox::NativeMailbox(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeMailbox::~NativeMailbox() { close(); }

std::variant<std::unique_ptr<NativeMailbox>, StoreError>
NativeMailbox::create(RecordStore store, std::array<std::byte, 20> key, MailboxLimits limits,
                      MemoryBudget budget, Executor executor, MailboxCallbacks callbacks,
                      LookupDeduplication deduplication) {
    if (!executor || !callbacks.on_record || !callbacks.on_error || !callbacks.on_catchup ||
        (deduplication != LookupDeduplication::IdAndContent &&
         deduplication != LookupDeduplication::Content)) {
        throw std::invalid_argument("native mailbox requires executor and callbacks");
    }
    auto impl = std::make_shared<Impl>(std::move(store), key, limits, std::move(budget),
                                       std::move(executor), std::move(callbacks), deduplication);
    auto watch = impl->store.watch(
        key,
        [weak = std::weak_ptr<Impl>(impl)](StoreBatch batch) {
            if (auto self = weak.lock(); self && self->active) {
                self->ingest(std::move(batch));
            }
        },
        deduplication == LookupDeduplication::Content);
    if (auto *error = std::get_if<StoreError>(&watch)) {
        return *error;
    }
    impl->watch.emplace(std::move(std::get<RecordWatch>(watch)));
    impl->registered = impl->watch->registration();
    return std::unique_ptr<NativeMailbox>(new NativeMailbox(std::move(impl)));
}

std::shared_future<std::size_t> NativeMailbox::registration() const { return impl_->registered; }

bool NativeMailbox::is_open() const noexcept { return impl_->active.load(); }
std::optional<StoreError> NativeMailbox::pending_store_error() const {
    std::lock_guard lock(impl_->submit_mutex);
    return impl_->store_error;
}

CatchupResult NativeMailbox::catch_up() {
    if (!impl_->active) {
        return CatchupError::Closed;
    }
    if (impl_->catching_up) {
        return CatchupError::AlreadyStarted;
    }
    if (impl_->registered.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return CatchupError::RegistrationPending;
    }
    try {
        if (impl_->registered.get() == 0) {
            return CatchupError::RegistrationFailed;
        }
    } catch (const std::exception &) {
        return CatchupError::RegistrationFailed;
    }
    const auto error = impl_->store.lookup(
        impl_->key,
        [weak = std::weak_ptr<Impl>(impl_)](LookupResult result) {
            if (auto self = weak.lock(); self && self->active) {
                self->catchup_complete(std::move(result));
            }
        },
        impl_->deduplication);
    if (error) {
        return *error;
    }
    impl_->catching_up = true;
    return std::monostate{};
}

void NativeMailbox::close() {
    if (impl_) {
        impl_->stop();
    }
}

} // namespace p2pdc::detail
