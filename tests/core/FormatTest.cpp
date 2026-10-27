#include <gtest/gtest.h>

#include "core/Format.h"

using lo::core::compactCount;
using lo::core::formatMessageArgs;

TEST(Format, Placeholders) {
    EXPECT_EQ(formatMessageArgs("Page {} of {}", 1, 3), "Page 1 of 3");
    EXPECT_EQ(formatMessageArgs("{1} before {0}", "a", "b"), "b before a");
    EXPECT_EQ(formatMessageArgs("{{literal}}"), "{literal}");
}

TEST(Format, UnknownPlaceholdersStay) { EXPECT_EQ(formatMessageArgs("{} and {}", "one"), "one and {}"); }

TEST(Format, CompactCount) {
    EXPECT_EQ(compactCount(999), "999");
    EXPECT_EQ(compactCount(1234), "1.2k");
    EXPECT_EQ(compactCount(1500000), "1.5M");
}
