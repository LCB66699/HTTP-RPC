#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "shared/cache/l1_cache.h"

TEST(L1Cache, GetNonexistentReturnsNullopt) {
    L1Cache cache(10, 30);
    EXPECT_EQ(cache.Get("missing"), std::nullopt);
    EXPECT_EQ(cache.MissCount(), 1);
    EXPECT_EQ(cache.HitCount(), 0);
}

TEST(L1Cache, SetAndGetReturnsValue) {
    L1Cache cache(10, 30);
    cache.Set("k1", "v1");
    auto v = cache.Get("k1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "v1");
    EXPECT_EQ(cache.HitCount(), 1);
    EXPECT_EQ(cache.MissCount(), 0);
}

TEST(L1Cache, SetUpdatesExistingKey) {
    L1Cache cache(10, 30);
    cache.Set("k1", "v1");
    cache.Set("k1", "v2");
    auto v = cache.Get("k1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "v2");
}

TEST(L1Cache, DeleteRemovesKey) {
    L1Cache cache(10, 30);
    cache.Set("k1", "v1");
    cache.Delete("k1");
    EXPECT_EQ(cache.Get("k1"), std::nullopt);
    EXPECT_EQ(cache.Size(), 0);
}

TEST(L1Cache, DeleteNonexistentIsSafe) {
    L1Cache cache(10, 30);
    EXPECT_NO_THROW(cache.Delete("no_such_key"));
    EXPECT_EQ(cache.Size(), 0);
}

TEST(L1Cache, ClearRemovesAll) {
    L1Cache cache(10, 30);
    cache.Set("k1", "v1");
    cache.Set("k2", "v2");
    cache.Set("k3", "v3");
    EXPECT_EQ(cache.Size(), 3);
    cache.Clear();
    EXPECT_EQ(cache.Size(), 0);
    EXPECT_EQ(cache.Get("k1"), std::nullopt);
}

TEST(L1Cache, TtlExpiryReturnsNullopt) {
    L1Cache cache(10, 1);  // 1 second TTL
    cache.Set("k1", "v1");
    EXPECT_TRUE(cache.Get("k1").has_value());
    EXPECT_EQ(cache.HitCount(), 1);

    // Wait for TTL to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto v = cache.Get("k1");
    EXPECT_EQ(v, std::nullopt);
    EXPECT_EQ(cache.MissCount(), 1);  // only the expired Get counts as miss
    EXPECT_EQ(cache.Size(), 0);
}

TEST(L1Cache, TtlExpiryCausesEvictionNotLru) {
    // capacity=2, so Set("k3") triggers eviction (size hits max)
    L1Cache cache(2, 1);
    cache.Set("k1", "v1");
    cache.Set("k2", "v2");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    // k1, k2 now expired

    cache.Set("k3", "v3");
    // EvictExpired fires first (lru_.size() >= max_entries_), removes both expired
    EXPECT_EQ(cache.Size(), 1);
    EXPECT_FALSE(cache.Get("k1").has_value());
    EXPECT_FALSE(cache.Get("k2").has_value());
    EXPECT_TRUE(cache.Get("k3").has_value());
}

TEST(L1Cache, LruEvictionAtCapacity) {
    L1Cache cache(2, 300);  // 300s TTL, won't expire during test

    cache.Set("k1", "v1");
    cache.Set("k2", "v2");
    EXPECT_EQ(cache.Size(), 2);

    // Access k1 to make k2 the LRU tail
    cache.Get("k1");

    // Insert k3 �?k2 should be evicted (LRU, not expired)
    cache.Set("k3", "v3");
    EXPECT_EQ(cache.Size(), 2);
    EXPECT_TRUE(cache.Get("k1").has_value());
    EXPECT_EQ(cache.Get("k2"), std::nullopt);
    EXPECT_TRUE(cache.Get("k3").has_value());
}

TEST(L1Cache, LruPromotionOnGet) {
    L1Cache cache(2, 300);

    cache.Set("k1", "v1");
    cache.Set("k2", "v2");

    // Access k2 twice �?it should be promoted to front
    cache.Get("k2");
    cache.Get("k2");

    // k1 is now LRU tail
    cache.Set("k3", "v3");
    EXPECT_TRUE(cache.Get("k2").has_value());
    EXPECT_EQ(cache.Get("k1"), std::nullopt);
    EXPECT_TRUE(cache.Get("k3").has_value());
}

TEST(L1Cache, SizeReflectsActualEntries) {
    L1Cache cache(10, 30);
    EXPECT_EQ(cache.Size(), 0);
    cache.Set("a", "1");
    EXPECT_EQ(cache.Size(), 1);
    cache.Set("b", "2");
    EXPECT_EQ(cache.Size(), 2);
    cache.Delete("a");
    EXPECT_EQ(cache.Size(), 1);
}

TEST(L1Cache, HitMissCountersAccurate) {
    L1Cache cache(10, 30);

    EXPECT_EQ(cache.HitCount(), 0);
    EXPECT_EQ(cache.MissCount(), 0);

    cache.Get("not_there");   // miss
    cache.Get("also_missing"); // miss
    cache.Set("k", "v");
    cache.Get("k");           // hit
    cache.Get("k");           // hit
    cache.Get("gone");        // miss

    EXPECT_EQ(cache.HitCount(), 2);
    EXPECT_EQ(cache.MissCount(), 3);
}

TEST(L1Cache, ConcurrentReadsAreSafe) {
    L1Cache cache(10, 300);
    cache.Set("shared", "value");

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&cache] {
            for (int j = 0; j < 100; ++j) {
                auto v = cache.Get("shared");
                if (v.has_value())
                    EXPECT_EQ(*v, "value");
            }
        });
    }
    for (auto &t : threads) t.join();

    EXPECT_EQ(cache.Size(), 1);
}

TEST(L1Cache, ConcurrentWritesAreSafe) {
    L1Cache cache(50, 300);

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&cache, i] {
            for (int j = 0; j < 50; ++j) {
                cache.Set("k" + std::to_string(i) + "_" + std::to_string(j),
                          "v" + std::to_string(j));
            }
        });
    }
    for (auto &t : threads) t.join();

    // All entries should be set (though some may be evicted if > capacity)
    EXPECT_GT(cache.Size(), 0);
    // No key collision: all keys are unique by thread_id + j
    EXPECT_LE(cache.Size(), 50);
}
