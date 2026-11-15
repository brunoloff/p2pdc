#include "../src/browser/browser_record_store.hpp"
#include "../src/discovery/native_mailbox.hpp"
#include "../src/discovery/native_publisher.hpp"
#include "p2pdc/runtime.h"

#include <emscripten.h>

#include <chrono>
#include <iostream>
#include <memory>

using namespace std::chrono_literals;
using namespace p2pdc;
using namespace p2pdc::detail;

bool portable_discovery_smoke();
bool record_store_smoke();
bool browser_terminal_retention_smoke();
bool browser_terminal_budget_smoke();
std::function<void()> start_browser_public_discovery(Runtime &, std::function<void(bool)>);
std::function<void()> run_browser_mailbox_attempt_smoke(Runtime &, std::function<void(bool)>);
std::function<void()> run_browser_mailbox_failure_smoke(Runtime &, std::function<void(bool)>);

EM_JS(void, report_store_result, (int success, int stage), {
    document.body.dataset.result = success ? 'pass' : 'fail';
    document.body.textContent = success ? 'pass'
                                        : 'fail: browser record store stage ' + stage + ' ' +
                                              (document.body.dataset.debug || "");
});

namespace {
template <typename Error, typename Function> bool throws(Function function) {
    try {
        function();
        return false;
    } catch (const Error &) {
        return true;
    }
}

struct Probe : std::enable_shared_from_this<Probe> {
    Runtime runtime;
    RecordStore store = make_browser_record_store("http://127.0.0.1:49412", {1024, 128, 65536});
    std::unique_ptr<NativeMailbox> mailbox;
    std::unique_ptr<NativePublisher> publisher;
    std::optional<RecordStore> offline;
    std::unique_ptr<RecordWatch> offline_watch;
    std::shared_ptr<Probe> keepalive;
    CancelTimer deadline;
    std::function<void()> cancel_public;
    std::function<void()> cancel_mailbox_attempt;
    std::function<void()> cancel_failure_test;
    std::array<std::byte, 20> key{std::byte{71}, std::byte{19}};
    Bytes value{std::byte{0}, std::byte{42}, std::byte{255}};
    bool done = false;
    bool caught_up = false;
    bool written = false;
    bool looked_up = false;
    bool offline_ready = false;
    bool public_started = false;
    bool public_ready = false;
    bool mailbox_attempt_started = false;
    bool mailbox_attempt_ready = false;
    bool failure_test_started = false;
    bool failure_test_ready = false;
    bool closing_test = false;
    unsigned records = 0;
    unsigned records_at_close = 0;
    int stage = 0;
    void start();
    void poll();
    void poll_offline();
    void publish();
    void message(Bytes value);
    void check_complete();
    void begin_public();
    void begin_mailbox_attempt();
    void begin_failure_test();
    void final_record();
    void finish(bool success);
    bool check_rejections();
    bool check_lookup_limit();
    bool setup_offline();
    MailboxCallbacks callbacks();
};

bool Probe::check_rejections() {
    if (!browser_terminal_retention_smoke())
        return false;
    if (!throws<std::invalid_argument>(
            [] { (void)make_browser_record_store("http://example.com", {1024, 8, 4096}); }) ||
        !throws<std::invalid_argument>(
            [] { (void)make_browser_record_store("http://127.0.0.1:49412", {0, 8, 4096}); }) ||
        store.put({}, {}, [](bool) {}) != StoreError::InvalidKey ||
        store.put(key, {}, {}) != StoreError::InvalidCallback ||
        store.put(key, Bytes(1025), [](bool) {}) != StoreError::ResourceLimit ||
        store.lookup(
            {}, [](LookupResult) {}, LookupDeduplication::Content) != StoreError::InvalidKey ||
        store.lookup(key, {}, LookupDeduplication::Content) != StoreError::InvalidCallback)
        return false;
    if (!check_lookup_limit())
        return false;
    auto canceled = store.watch(key, [](StoreBatch) {}, false);
    if (!std::holds_alternative<RecordWatch>(canceled))
        return false;
    auto watch = std::move(std::get<RecordWatch>(canceled));
    auto registration = watch.registration();
    watch.cancel();
    return registration.wait_for(0ms) == std::future_status::ready && registration.get() == 0;
}

bool Probe::check_lookup_limit() {
    auto limited =
        make_browser_record_store("http://127.0.0.1:1", {1024, 8, 16 * 1024 * 1024}, 1000);
    return !limited.lookup(
               key, [](LookupResult) {}, LookupDeduplication::Content) &&
           limited.lookup(
               key, [](LookupResult) {}, LookupDeduplication::Content) == StoreError::ResourceLimit;
}

MailboxCallbacks Probe::callbacks() {
    auto weak = weak_from_this();
    return {[weak](Bytes bytes) {
                if (auto self = weak.lock())
                    self->message(std::move(bytes));
            },
            [weak](MailboxError error, std::optional<StoreError> detail) {
                if (auto self = weak.lock()) {
                    self->stage = 100 + 10 * static_cast<int>(error) +
                                  (detail ? static_cast<int>(*detail) : 9);
                    self->finish(false);
                }
            },
            [weak](bool ok) {
                if (auto self = weak.lock()) {
                    if (ok)
                        self->publish();
                    else
                        self->finish(false);
                }
            }};
}
bool Probe::setup_offline() {
    offline.emplace(make_browser_record_store("http://127.0.0.1:1", {1024, 8, 4096}, 1000));
    auto failed_watch = offline->watch(key, [](StoreBatch) {}, false);
    if (!std::holds_alternative<RecordWatch>(failed_watch))
        return false;
    offline_watch = std::make_unique<RecordWatch>(std::move(std::get<RecordWatch>(failed_watch)));
    return true;
}
void Probe::start() {
    stage = 1;
    keepalive = shared_from_this();
    auto weak = weak_from_this();
    if (!check_rejections()) {
        finish(false);
        return;
    }
    auto created = NativeMailbox::create(store, key, {4096, 32, 1024}, MemoryBudget(65536),
                                         runtime.executor(), callbacks());
    if (!std::holds_alternative<std::unique_ptr<NativeMailbox>>(created)) {
        finish(false);
        return;
    }
    mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(created));
    publisher =
        std::make_unique<NativePublisher>(store, key, MemoryBudget(65536), runtime.executor());
    if (!setup_offline()) {
        finish(false);
        return;
    }
    deadline = runtime.scheduler()(15s, [weak] {
        if (auto self = weak.lock())
            self->finish(false);
    });
    poll_offline();
    poll();
}

void Probe::begin_public() {
    auto weak = weak_from_this();
    cancel_public = start_browser_public_discovery(runtime, [weak](bool success) {
        if (auto self = weak.lock()) {
            if (!success) {
                self->stage = 47;
                self->finish(false);
            } else {
                self->public_ready = true;
                self->check_complete();
            }
        }
    });
}
void Probe::begin_mailbox_attempt() {
    auto weak = weak_from_this();
    cancel_mailbox_attempt = run_browser_mailbox_attempt_smoke(runtime, [weak](bool success) {
        if (auto self = weak.lock()) {
            if (!success) {
                self->stage = 50;
                self->finish(false);
            } else {
                self->mailbox_attempt_ready = true;
                self->check_complete();
            }
        }
    });
}
void Probe::begin_failure_test() {
    auto weak = weak_from_this();
    cancel_failure_test = run_browser_mailbox_failure_smoke(runtime, [weak](bool success) {
        if (auto self = weak.lock()) {
            if (!success) {
                self->stage = 51;
                self->finish(false);
            } else {
                self->failure_test_ready = true;
                self->check_complete();
            }
        }
    });
}

void Probe::poll() {
    stage = 2;
    if (done)
        return;
    if (mailbox->registration().wait_for(0ms) != std::future_status::ready) {
        auto weak = weak_from_this();
        (void)runtime.scheduler()(20ms, [weak] {
            if (auto self = weak.lock())
                self->poll();
        });
        return;
    }
    if (mailbox->registration().get() == 0 ||
        !std::holds_alternative<std::monostate>(mailbox->catch_up()))
        finish(false);
}

void Probe::poll_offline() {
    if (done)
        return;
    auto registration = offline_watch->registration();
    if (registration.wait_for(0ms) != std::future_status::ready) {
        auto weak = weak_from_this();
        (void)runtime.scheduler()(20ms, [weak] {
            if (auto self = weak.lock())
                self->poll_offline();
        });
        return;
    }
    if (registration.get() != 0) {
        stage = 46;
        finish(false);
        return;
    }
    offline_ready = true;
    offline_watch->cancel();
    check_complete();
}

void Probe::publish() {
    stage = 3;
    if (done || caught_up) {
        finish(false);
        return;
    }
    caught_up = true;
    auto weak = weak_from_this();
    auto result = publisher->send(value, [weak](bool success) {
        if (auto self = weak.lock()) {
            if (!success) {
                self->stage = 45;
                self->finish(false);
            } else {
                self->written = true;
                self->check_complete();
            }
        }
    });
    if (!std::holds_alternative<std::monostate>(result))
        finish(false);
}

void Probe::message(Bytes bytes) {
    stage = 4;
    if (done || bytes != value) {
        stage = 41;
        finish(false);
        return;
    }
    if (++records > 1)
        return;
    auto weak = weak_from_this();
    auto error = store.lookup(
        key,
        [weak](LookupResult result) {
            if (auto self = weak.lock()) {
                self->looked_up = result.backend_success && !result.batch.error &&
                                  result.batch.values == std::vector<Bytes>{self->value};
                if (!self->looked_up) {
                    self->stage = 42;
                    self->finish(false);
                } else
                    self->check_complete();
            }
        },
        LookupDeduplication::Content);
    if (error) {
        stage = 43;
        finish(false);
    }
}

void Probe::check_complete() {
    if (done || closing_test || !written || !looked_up || !offline_ready)
        return;
    if (!public_started) {
        public_started = true;
        records_at_close = records;
        mailbox->close();
        begin_public();
        return;
    }
    if (!public_ready)
        return;
    if (!mailbox_attempt_started) {
        mailbox_attempt_started = true;
        begin_mailbox_attempt();
        return;
    }
    if (!mailbox_attempt_ready)
        return;
    if (!failure_test_started) {
        failure_test_started = true;
        begin_failure_test();
        return;
    }
    if (!failure_test_ready)
        return;
    final_record();
}
void Probe::final_record() {
    closing_test = true;
    stage = 5;
    auto weak = weak_from_this();
    auto error = store.put(key, Bytes{std::byte{7}}, [weak](bool success) {
        if (auto self = weak.lock()) {
            (void)self->runtime.scheduler()(100ms, [weak, success] {
                if (auto owner = weak.lock())
                    owner->finish(success && owner->records == owner->records_at_close);
            });
        }
    });
    if (error)
        finish(false);
}

void Probe::finish(bool success) {
    if (done)
        return;
    done = true;
    if (deadline)
        deadline();
    if (cancel_public)
        cancel_public();
    if (cancel_mailbox_attempt)
        cancel_mailbox_attempt();
    if (cancel_failure_test)
        cancel_failure_test();
    if (mailbox)
        mailbox->close();
    if (publisher)
        publisher->close();
    if (offline_watch)
        offline_watch->cancel();
    report_store_result(success && browser_terminal_budget_smoke(), stage);
    keepalive.reset();
}
} // namespace

int main() {
    try {
        if (!portable_discovery_smoke() || !record_store_smoke()) {
            report_store_result(0, -2);
            return 1;
        }
        auto probe = std::make_shared<Probe>();
        probe->start();
    } catch (const std::exception &error) {
        std::cerr << "browser record store smoke: " << error.what() << '\n';
        report_store_result(0, -1);
    }
}
