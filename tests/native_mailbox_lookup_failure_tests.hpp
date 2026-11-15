#include <atomic>

namespace {
TEST_F(NativeMailboxTest, LookupRecordErrorDoesNotReportCatchupFailure) {
    ASSERT_TRUE(put(Bytes{std::byte{42}}));
    auto fail = std::make_shared<std::atomic_bool>(false);
    NativeStore store(second, store_limits, [fail] {
        if (fail->load())
            throw std::bad_alloc();
    });
    auto result = NativeMailbox::create(std::move(store), address, {4096, 16, 1024},
                                        MemoryBudget(4096), loop.executor(), callbacks());
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<NativeMailbox>>(result));
    auto mailbox = std::move(std::get<std::unique_ptr<NativeMailbox>>(result));
    ASSERT_EQ(mailbox->registration().wait_for(10s), std::future_status::ready);
    ASSERT_NE(mailbox->registration().get(), 0U);
    ASSERT_TRUE(loop.until([&] { return !records.empty(); }));
    fail->store(true);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(mailbox->catch_up()));
    ASSERT_TRUE(loop.until([&] { return !errors.empty(); }));
    loop.drain();
    EXPECT_EQ(errors, (std::vector{MailboxError::StoreFailure}));
    EXPECT_FALSE(caught_up.has_value());
    EXPECT_FALSE(mailbox->is_open());
}

TEST(NativeMailboxValidation, RejectsUnknownDeduplicationPolicy) {
    auto runner = std::make_shared<dht::DhtRunner>();
    NativeStore store(runner, {4, 1, 4});
    MailboxCallbacks callbacks{[](Bytes) {}, [](MailboxError, std::optional<StoreError>) {},
                               [](bool) {}};
    EXPECT_THROW((NativeMailbox::create(
                     store, address, {4, 1, 4}, MemoryBudget(4), [](auto) {}, callbacks,
                     static_cast<LookupDeduplication>(99))),
                 std::invalid_argument);
}
} // namespace
