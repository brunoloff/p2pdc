#include "../src/discovery/record_store.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

bool record_store_smoke();

namespace {
class InvalidWatch final : public p2pdc::detail::RecordWatchBackend {
  public:
    void cancel() override {}
    std::shared_future<std::size_t> registration() const override { return {}; }
};
} // namespace

TEST(RecordStore, MailboxAndPublisherUsePortableBackend) { EXPECT_TRUE(record_store_smoke()); }

TEST(RecordStore, RejectsNullBackends) {
    EXPECT_THROW(static_cast<void>(p2pdc::detail::RecordStore(nullptr)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(p2pdc::detail::RecordWatch(nullptr)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(p2pdc::detail::RecordWatch(std::make_unique<InvalidWatch>())),
                 std::invalid_argument);
}
