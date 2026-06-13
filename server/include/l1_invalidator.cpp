#include "l1_invalidator.h"
#include "l1_cache.h"
#include "redis_client.h"
#include <cstdio>

L1CacheInvalidator::L1CacheInvalidator(L1Cache* cache, RedisClient* redis)
    : cache_(cache), redis_(redis) {}

L1CacheInvalidator::~L1CacheInvalidator() { Stop(); }

void L1CacheInvalidator::Start() {
    if (!redis_ || !cache_) return;
    running_ = true;
    redis_->SubscribeStandalone("cache:invalidate",
        [this](const std::string& /*channel*/, const std::string& msg) {
            // msg is the cache key to invalidate, e.g. "u:5:sheet:123"
            if (cache_) cache_->Delete(msg);
            fprintf(stderr, "[L1] Invalidated by Pub/Sub: %s\n", msg.c_str());
        });
    printf("[L1] Cache invalidator started, listening on cache:invalidate\n");
}

void L1CacheInvalidator::Stop() {
    running_ = false;
    if (sub_thread_.joinable()) sub_thread_.join();
}
