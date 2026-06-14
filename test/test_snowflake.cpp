#include <gtest/gtest.h>
#include <set>
#include "../server/include/snowflake.h"

TEST(Snowflake, GeneratesUniqueIDs) {
    Snowflake sf(1);
    std::set<int64_t> ids;
    for (int i = 0; i < 1000; i++) {
        int64_t id = sf.Next();
        EXPECT_GT(id, 0);
        EXPECT_TRUE(ids.insert(id).second) << "duplicate ID: " << id;
    }
}

TEST(Snowflake, MonotonicallyIncreasing) {
    Snowflake sf(5);
    int64_t prev = 0;
    for (int i = 0; i < 100; i++) {
        int64_t id = sf.Next();
        EXPECT_GT(id, prev);
        prev = id;
    }
}

TEST(Snowflake, DifferentWorkersProduceDifferentIDs) {
    Snowflake sf1(1), sf2(2);
    int64_t id1 = sf1.Next();
    int64_t id2 = sf2.Next();
    EXPECT_NE(id1, id2);
}

TEST(Snowflake, WorkerIDRange) {
    // Worker IDs 0-31 should all work
    for (int w = 0; w <= 31; w++) {
        Snowflake sf(w);
        EXPECT_GT(sf.Next(), 0);
    }
}
