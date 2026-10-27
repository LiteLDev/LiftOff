#include <gtest/gtest.h>

#include "core/Glob.h"

using lo::core::globMatch;

TEST(Glob, Literal) {
    EXPECT_TRUE(globMatch("foundation.i18n", "foundation.i18n"));
    EXPECT_FALSE(globMatch("foundation.i18n", "foundation.blocks"));
}

TEST(Glob, Wildcards) {
    EXPECT_TRUE(globMatch("*", ""));
    EXPECT_TRUE(globMatch("foundation.*", "foundation.world_db"));
    EXPECT_TRUE(globMatch("*.i18n", "foundation.i18n"));
    EXPECT_TRUE(globMatch("f?undation.*", "foundation.x"));
    EXPECT_FALSE(globMatch("foundation.?", "foundation."));
    EXPECT_FALSE(globMatch("unit.*", "foundation.blocks"));
}
