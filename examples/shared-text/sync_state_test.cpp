#include "sync_state.hpp"

#include <gtest/gtest.h>

using namespace shared_text;
TEST(PeerSync, SendsIncrementalUpdatesAndDeletionsAfterCatchUp) {
    Document source, target;
    PeerSync sync;
    ASSERT_TRUE(source.replace(QString(4096, 'a')));
    auto initial = sync.prepare(source);
    const auto initialSize = initial.data.size();
    ASSERT_TRUE(target.apply(initial.data));
    sync.accepted(std::move(initial));
    ASSERT_TRUE(source.replace(source.text() + "new"));
    auto delta = sync.prepare(source);
    EXPECT_LT(delta.data.size(), initialSize / 2);
    ASSERT_TRUE(target.apply(delta.data));
    sync.accepted(std::move(delta));
    ASSERT_TRUE(source.replace("new"));
    auto deletion = sync.prepare(source);
    ASSERT_TRUE(target.apply(deletion.data));
    sync.accepted(std::move(deletion));
    EXPECT_EQ(target.text(), source.text());
}
TEST(PeerSync, RejectedSendRetainsBaselineAndResetCatchesUpNewPeer) {
    Document source, target, newcomer;
    PeerSync sync;
    ASSERT_TRUE(source.replace("first"));
    auto initial = sync.prepare(source);
    ASSERT_TRUE(target.apply(initial.data));
    sync.accepted(std::move(initial));
    ASSERT_TRUE(source.replace("first second"));
    auto rejected = sync.prepare(source);
    EXPECT_FALSE(rejected.data.empty());
    ASSERT_TRUE(source.replace("first second third"));
    auto retry = sync.prepare(source);
    ASSERT_TRUE(target.apply(retry.data));
    sync.accepted(std::move(retry));
    EXPECT_EQ(target.text(), source.text());
    sync.reset();
    auto catchup = sync.prepare(source);
    ASSERT_TRUE(newcomer.apply(catchup.data));
    EXPECT_EQ(newcomer.text(), source.text());
}
