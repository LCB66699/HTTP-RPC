#include <gtest/gtest.h>
#include "shared/helper/cache_helpers.h"

TEST(SheetHelpers, VersionKey) {
    EXPECT_EQ(SheetVersionKey(7), "u:7:sheets:version");
    EXPECT_EQ(SheetVersionKey(0), "u:0:sheets:version");
}

TEST(SheetHelpers, ListCacheKeyNoLimit) {
    auto key = SheetListCacheKey(5, 3, 0, "");
    EXPECT_EQ(key, "u:5:sheets:v3");
}

TEST(SheetHelpers, ListCacheKeyWithLimit) {
    auto key = SheetListCacheKey(5, 3, 20, "");
    EXPECT_EQ(key, "u:5:sheets:v3:l20");
}

TEST(SheetHelpers, ListCacheKeyWithCursor) {
    auto key = SheetListCacheKey(5, 3, 20, "42");
    EXPECT_EQ(key, "u:5:sheets:v3:l20:c42");
}

TEST(SheetHelpers, CacheKey) {
    EXPECT_EQ(SheetCacheKey(7, 42), "u:7:sheet:42");
}

TEST(SheetHelpers, LockKey) {
    EXPECT_EQ(SheetLockKey(7, 42), "lock:u:7:sheet:42");
}
