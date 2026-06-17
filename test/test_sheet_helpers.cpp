#include <gtest/gtest.h>
#include "../server/include/sheet_helpers.h"

TEST(SheetHelpers, VersionKey) {
    EXPECT_EQ(SheetVersionKey(7), "u:7:sheets:version");
    EXPECT_EQ(SheetVersionKey(0), "u:0:sheets:version");
}

TEST(SheetHelpers, ListCacheKeyNoPage) {
    auto key = SheetListCacheKey(5, 3, 0, 0);
    EXPECT_EQ(key, "u:5:sheets:v3");
}

TEST(SheetHelpers, ListCacheKeyWithPage) {
    auto key = SheetListCacheKey(5, 3, 2, 10);
    EXPECT_EQ(key, "u:5:sheets:v3:p2:ps10");
}

TEST(SheetHelpers, CacheKey) {
    EXPECT_EQ(SheetCacheKey(7, 42), "u:7:sheet:42");
}

TEST(SheetHelpers, LockKey) {
    EXPECT_EQ(SheetLockKey(7, 42), "lock:u:7:sheet:42");
}
