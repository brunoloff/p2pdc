#include "document.hpp"
#include <gtest/gtest.h>

using shared_text::Document;
TEST(SharedDocument, ConcurrentEditsConvergeAndCatchUp) {
    Document a, b;
    ASSERT_TRUE(a.replace(QString::fromUtf8("Hi 🌍")));
    ASSERT_TRUE(b.apply(a.difference(b.stateVector())));
    ASSERT_TRUE(a.replace(QString::fromUtf8("Hi 🌍!")));
    ASSERT_TRUE(b.replace(QString::fromUtf8("Hello 🌍")));
    auto aUpdate = a.difference(b.stateVector());
    auto bUpdate = b.difference(a.stateVector());
    ASSERT_TRUE(a.apply(bUpdate));
    ASSERT_TRUE(b.apply(aUpdate));
    EXPECT_EQ(a.text(), b.text());
    EXPECT_TRUE(a.text().contains("Hello"));
    EXPECT_TRUE(a.text().contains("!"));
}
TEST(SharedDocument, RejectsInvalidTextAndUpdates) {
    Document document;
    ASSERT_TRUE(document.replace("safe"));
    EXPECT_FALSE(document.replace(QString(QChar::Null)));
    EXPECT_FALSE(document.replace(QString(65537, 'a')));
    EXPECT_FALSE(document.apply({}));
    EXPECT_EQ(document.text(), "safe");
    EXPECT_THROW((void)document.difference({}), std::invalid_argument);
}
TEST(SharedDocument, ReplacesUnicodeWithoutSplittingSurrogatePairs) {
    Document a, b;
    ASSERT_TRUE(a.replace(QString::fromUtf8("one 😀 tail")));
    ASSERT_TRUE(b.apply(a.difference(b.stateVector())));
    ASSERT_TRUE(a.replace(QString::fromUtf8("one 😁 tail")));
    auto update = a.difference(b.stateVector());
    ASSERT_TRUE(b.apply(update));
    ASSERT_TRUE(b.apply(update));
    EXPECT_EQ(a.text(), QString::fromUtf8("one 😁 tail"));
    EXPECT_EQ(a.text(), b.text());
}
