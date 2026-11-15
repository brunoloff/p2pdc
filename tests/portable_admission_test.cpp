#include <gtest/gtest.h>

bool portable_admission_smoke();

TEST(PeerAdmission, CommonValidationAcceptsAndRejectsExpectedBindings) {
    EXPECT_TRUE(portable_admission_smoke());
}
