#include <gtest/gtest.h>
#include "shared/helper/cache_helpers.h"

TEST(FileHelpers, VersionKey) {
    EXPECT_EQ(FileVersionKey(7), "u:7:files:version");
}

TEST(FileHelpers, ListCacheKeyNoLimit) {
    auto key = FileListCacheKey(5, 2, 0, "");
    EXPECT_EQ(key, "u:5:files:v2");
}

TEST(FileHelpers, ListCacheKeyWithLimit) {
    auto key = FileListCacheKey(5, 2, 20, "");
    EXPECT_EQ(key, "u:5:files:v2:l20");
}

TEST(FileHelpers, ListCacheKeyWithCursor) {
    auto key = FileListCacheKey(5, 2, 20, "99");
    EXPECT_EQ(key, "u:5:files:v2:l20:c99");
}

TEST(FileHelpers, CacheKey) {
    EXPECT_EQ(FileCacheKey(3, 99), "u:3:file:99");
}

TEST(FileHelpers, LockKey) {
    EXPECT_EQ(FileLockKey(3, 99), "lock:u:3:file:99");
}
