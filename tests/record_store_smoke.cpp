#include "../src/discovery/native_mailbox.hpp"
#include "../src/discovery/native_publisher.hpp"
#include "../src/discovery/native_signal_pipe.hpp"

#include <deque>
#include <stdexcept>

using namespace p2pdc;
using namespace p2pdc::detail;

namespace {
template <typename Error, typename Function> bool throws(Function function) {
    try {
        function();
        return false;
    } catch (const Error &) {
        return true;
    }
}

struct FakeWatchState {
    std::promise<std::size_t> ready;
    std::shared_future<std::size_t> registration = ready.get_future().share();
    std::function<void(StoreBatch)> callback;
    bool active = true;
};

class FakeWatch final : public RecordWatchBackend {
  public:
    explicit FakeWatch(std::shared_ptr<FakeWatchState> state) : state_(std::move(state)) {}
    ~FakeWatch() override { cancel(); }
    void cancel() override { state_->active = false; }
    std::shared_future<std::size_t> registration() const override { return state_->registration; }

  private:
    std::shared_ptr<FakeWatchState> state_;
};

class FakeStore final : public RecordStoreBackend {
  public:
    mutable std::shared_ptr<FakeWatchState> active;
    mutable std::vector<Bytes> records;
    mutable std::deque<std::function<void(bool)>> pending;
    bool hold = false;
    std::optional<StoreError> put(ByteView key, ByteView payload,
                                  std::function<void(bool)> completion) const override;
    std::optional<StoreError> lookup(ByteView key, std::function<void(LookupResult)> completion,
                                     LookupDeduplication deduplication) const override;
    std::variant<RecordWatch, StoreError>
    watch(ByteView key, std::function<void(StoreBatch)> callback, bool deduplicate) const override;
};

std::optional<StoreError> FakeStore::put(ByteView key, ByteView payload,
                                         std::function<void(bool)> completion) const {
    if (key.empty())
        return StoreError::InvalidKey;
    if (!completion)
        return StoreError::InvalidCallback;
    records.emplace_back(payload.begin(), payload.end());
    if (active && active->active)
        active->callback(StoreBatch{{records.back()}, {}, false});
    if (hold)
        pending.push_back(std::move(completion));
    else
        completion(true);
    return std::nullopt;
}
std::optional<StoreError> FakeStore::lookup(ByteView key,
                                            std::function<void(LookupResult)> completion,
                                            LookupDeduplication) const {
    if (key.empty())
        return StoreError::InvalidKey;
    if (!completion)
        return StoreError::InvalidCallback;
    completion(LookupResult{StoreBatch{records, {}, false}, true});
    return std::nullopt;
}
std::variant<RecordWatch, StoreError>
FakeStore::watch(ByteView key, std::function<void(StoreBatch)> callback, bool) const {
    if (key.empty())
        return StoreError::InvalidKey;
    if (!callback)
        return StoreError::InvalidCallback;
    active = std::make_shared<FakeWatchState>();
    active->callback = std::move(callback);
    return RecordWatch(std::make_unique<FakeWatch>(active));
}

struct Harness {
    std::shared_ptr<FakeStore> backend = std::make_shared<FakeStore>();
    RecordStore store{backend};
    std::deque<std::function<void()>> tasks;
    std::vector<Bytes> received;
    std::vector<MailboxError> errors;
    std::array<std::byte, 20> key{std::byte{1}};
    bool caught_up = false;
    bool published = false;
    Executor executor() {
        return [this](auto task) { tasks.push_back(std::move(task)); };
    }
    void drain();
    void complete_next();
    bool exchange();
    bool reject_and_close();
    bool watch_edges();
    bool signaling_pipe();
    bool concurrent_signaling();
    bool constrained_signaling();
    bool mixed_size_signaling();
};

void Harness::drain() {
    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
}
void Harness::complete_next() {
    auto done = std::move(backend->pending.front());
    backend->pending.pop_front();
    done(true);
    drain();
}
bool Harness::exchange() {
    backend->records.push_back(Bytes{std::byte{1}});
    auto created = NativeMailbox::create(
        store, key, {128, 8, 32}, MemoryBudget(1024), executor(),
        {[this](Bytes value) { received.push_back(std::move(value)); },
         [this](MailboxError error, std::optional<StoreError>) { errors.push_back(error); },
         [this](bool value) { caught_up = value; }});
    if (!std::holds_alternative<std::unique_ptr<NativeMailbox>>(created))
        return false;
    auto mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(created));
    if (mailbox->catch_up() != CatchupResult{CatchupError::RegistrationPending})
        return false;
    backend->active->ready.set_value(1);
    if (!std::holds_alternative<std::monostate>(mailbox->catch_up()))
        return false;
    drain();
    if (!caught_up || received != std::vector<Bytes>{Bytes{std::byte{1}}})
        return false;
    NativePublisher publisher(store, key, MemoryBudget(1024), executor());
    const Bytes value{std::byte{2}, std::byte{255}};
    if (!std::holds_alternative<std::monostate>(
            publisher.send(value, [this](bool ok) { published = ok; })))
        return false;
    drain();
    if (!published || received.size() != 2 || received.back() != value || !errors.empty())
        return false;
    mailbox->close();
    return !backend->active->active;
}
bool Harness::reject_and_close() {
    if (store.put({}, {}, [](bool) {}) != StoreError::InvalidKey)
        return false;
    if (store.put(key, {}, {}) != StoreError::InvalidCallback)
        return false;
    NativePublisher publisher(store, key, MemoryBudget(1), executor());
    auto rejected = publisher.send(Bytes(2), [](bool) {});
    if (!std::holds_alternative<PublishError>(rejected) ||
        std::get<PublishError>(rejected) != PublishError::ResourceLimit)
        return false;
    publisher.close();
    return std::get<PublishError>(publisher.send({}, [](bool) {})) == PublishError::Closed;
}
bool Harness::watch_edges() {
    if (!throws<std::invalid_argument>([] { (void)RecordStore(nullptr); }) ||
        !throws<std::invalid_argument>([] { (void)RecordWatch(nullptr); }))
        return false;
    if (store.lookup(
            {}, [](LookupResult) {}, LookupDeduplication::Content) != StoreError::InvalidKey ||
        store.lookup(key, {}, LookupDeduplication::Content) != StoreError::InvalidCallback)
        return false;
    auto bad_key = store.watch({}, [](StoreBatch) {}, false);
    auto bad_callback = store.watch(key, {}, false);
    if (!std::holds_alternative<StoreError>(bad_key) ||
        std::get<StoreError>(bad_key) != StoreError::InvalidKey ||
        !std::holds_alternative<StoreError>(bad_callback) ||
        std::get<StoreError>(bad_callback) != StoreError::InvalidCallback)
        return false;
    auto created = store.watch(key, [](StoreBatch) {}, false);
    if (!std::holds_alternative<RecordWatch>(created))
        return false;
    auto original = std::move(std::get<RecordWatch>(created));
    auto watch = std::move(original);
    if (!throws<std::logic_error>([&] { (void)original.registration(); }) ||
        !throws<std::logic_error>([&] { original.cancel(); }))
        return false;
    backend->active->ready.set_value(1);
    if (watch.registration().get() != 1)
        return false;
    watch.cancel();
    watch.cancel();
    if (backend->active->active)
        return false;
    auto copy = store;
    auto moved = std::move(copy);
    return throws<std::logic_error>([&] { (void)copy.put(key, {}, [](bool) {}); }) &&
           throws<std::logic_error>([&] {
               (void)copy.lookup(key, [](LookupResult) {}, LookupDeduplication::Content);
           }) &&
           throws<std::logic_error>([&] { (void)copy.watch(key, [](StoreBatch) {}, false); }) &&
           moved.put({}, {}, [](bool) {}) == StoreError::InvalidKey;
}
bool Harness::signaling_pipe() {
    SignalPipeCallbacks callbacks{
        [this](Bytes value) { received.push_back(std::move(value)); },
        [this](MailboxError error, std::optional<StoreError>) { errors.push_back(error); },
        [this](OutboxError) { errors.push_back(MailboxError::StoreFailure); },
        [this] { published = true; }, [this](bool ok) { caught_up = ok; }};
    const SignalPipeLimits limits{{128, 8, 32}, {128, 8, 32}};
    auto invalid =
        NativeSignalPipe::create(store, key, {}, MemoryBudget(1024), limits, executor(), callbacks);
    if (!std::holds_alternative<StoreError>(invalid) ||
        std::get<StoreError>(invalid) != StoreError::InvalidKey)
        return false;
    auto created = NativeSignalPipe::create(store, key, key, MemoryBudget(1024), limits, executor(),
                                            callbacks);
    auto value = std::get_if<std::unique_ptr<NativeSignalPipe>>(&created);
    if (!value)
        return false;
    auto pipe = std::move(*value);
    backend->active->ready.set_value(1);
    if (!std::holds_alternative<std::monostate>(pipe->catch_up()) ||
        pipe->send(Bytes{std::byte{7}}) != SignalingQueueResult::Accepted)
        return false;
    drain();
    if (!caught_up || !published || received != std::vector<Bytes>{Bytes{std::byte{7}}} ||
        !errors.empty())
        return false;
    pipe->close();
    return !pipe->is_open() && pipe->send({}) == SignalingQueueResult::Closed;
}
bool Harness::concurrent_signaling() {
    backend->hold = true;
    MemoryBudget budget(1024);
    const SignalPipeLimits limits{{128, 8, 32}, {4, 3, 4}};
    SignalPipeCallbacks callbacks{
        [](Bytes) {}, [](MailboxError, std::optional<StoreError>) {},
        [this](OutboxError) { errors.push_back(MailboxError::StoreFailure); },
        [this] { published = true; }, [](bool) {}};
    auto created = NativeSignalPipe::create(store, key, key, budget, limits, executor(), callbacks);
    auto pipe = std::move(std::get<std::unique_ptr<NativeSignalPipe>>(created));
    if (pipe->send(Bytes{std::byte{1}}) != SignalingQueueResult::Accepted ||
        pipe->send(Bytes{std::byte{2}}) != SignalingQueueResult::Accepted ||
        pipe->send(Bytes(3)) != SignalingQueueResult::Full ||
        pipe->send(Bytes(2)) != SignalingQueueResult::Accepted || backend->pending.size() != 3 ||
        pipe->send(Bytes{std::byte{4}}) != SignalingQueueResult::Full ||
        pipe->send(Bytes(5)) != SignalingQueueResult::TooLarge)
        return false;
    auto first = std::move(backend->pending.front());
    backend->pending.pop_front();
    first(false);
    drain();
    auto second = std::move(backend->pending.back());
    backend->pending.pop_back();
    second(true);
    drain();
    if (errors.size() != 1 || !published || !pipe->retry() || backend->pending.size() != 2)
        return false;
    published = false;
    pipe->close();
    while (!backend->pending.empty()) {
        auto done = std::move(backend->pending.front());
        backend->pending.pop_front();
        done(true);
    }
    drain();
    return !published && budget.used() == 0 && !pipe->retry();
}
bool Harness::constrained_signaling() {
    backend->hold = true;
    MemoryBudget budget(6);
    const SignalPipeLimits limits{{128, 8, 32}, {4, 2, 2}};
    unsigned completed = 0;
    SignalPipeCallbacks callbacks{
        [](Bytes) {}, [](MailboxError, std::optional<StoreError>) {},
        [this](OutboxError) { errors.push_back(MailboxError::StoreFailure); },
        [&completed] { ++completed; }, [](bool) {}};
    auto created = NativeSignalPipe::create(store, key, key, budget, limits, executor(), callbacks);
    auto pipe = std::move(std::get<std::unique_ptr<NativeSignalPipe>>(created));
    backend->active->active = false; // The peer's mailbox is separate from this local watch.
    if (pipe->send(Bytes(2, std::byte{1})) != SignalingQueueResult::Accepted ||
        pipe->send(Bytes(2, std::byte{2})) != SignalingQueueResult::Accepted ||
        pipe->send(Bytes{std::byte{3}}) != SignalingQueueResult::Full ||
        backend->pending.size() != 1 || !errors.empty())
        return false;
    complete_next();
    if (completed != 1 || backend->pending.size() != 1 || !errors.empty())
        return false;
    complete_next();
    pipe->close();
    return completed == 2 && errors.empty() && budget.used() == 0;
}
bool Harness::mixed_size_signaling() {
    backend->hold = true;
    MemoryBudget budget(16);
    const SignalPipeLimits limits{{128, 8, 32}, {12, 4, 4}};
    unsigned completed = 0;
    SignalPipeCallbacks callbacks{
        [](Bytes) {}, [](MailboxError, std::optional<StoreError>) {},
        [this](OutboxError) { errors.push_back(MailboxError::StoreFailure); },
        [&completed] { ++completed; }, [](bool) {}};
    auto created = NativeSignalPipe::create(store, key, key, budget, limits, executor(), callbacks);
    auto pipe = std::move(std::get<std::unique_ptr<NativeSignalPipe>>(created));
    backend->active->active = false;
    for (std::size_t size = 1; size <= 4; ++size)
        if (pipe->send(Bytes(size)) != SignalingQueueResult::Accepted)
            return false;
    if (backend->pending.size() != 3 || budget.used() != 16)
        return false;
    complete_next();
    pipe->retry_budget();
    if (backend->pending.size() != 2 || !errors.empty())
        return false;
    complete_next();
    pipe->retry_budget();
    if (backend->pending.size() != 2 || !errors.empty())
        return false;
    while (!backend->pending.empty())
        complete_next();
    pipe->close();
    return completed == 4 && errors.empty() && budget.used() == 0;
}
} // namespace

bool record_store_smoke() {
    Harness first, second, third, fourth, fifth, sixth, seventh;
    return first.exchange() && second.reject_and_close() && third.watch_edges() &&
           fourth.signaling_pipe() && fifth.concurrent_signaling() &&
           sixth.constrained_signaling() && seventh.mixed_size_signaling();
}
