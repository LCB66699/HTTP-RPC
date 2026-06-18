#include <gtest/gtest.h>
#include "../server/include/file_helpers.h"

TEST(FileHelpers, VersionKey) {
    EXPECT_EQ(FileVersionKey(7), "u:7:files:version");
}

TEST(FileHelpers, ListCacheKeyDefault) {
    auto key = FileListCacheKey(5, 2, 0, 0);
    EXPECT_EQ(key, "u:5:files:v2");
}

TEST(FileHelpers, ListCacheKeyPaginated) {
    auto key = FileListCacheKey(5, 2, 1, 20);
    EXPECT_EQ(key, "u:5:files:v2:p1:ps20");
}

TEST(FileHelpers, CacheKey) {
    EXPECT_EQ(FileCacheKey(3, 99), "u:3:file:99");
}

TEST(FileHelpers, LockKey) {
    EXPECT_EQ(FileLockKey(3, 99), "lock:u:3:file:99");
}
