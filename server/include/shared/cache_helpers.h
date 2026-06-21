#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>

// 通用缓存 key 生成 — sheet / file 共用，消除重复代码
inline std::string ResourceVersionKey(const char *resource, int64_t user_id) {
    return "u:" + std::to_string(user_id) + ":" + resource + "s:version";
}
inline std::string ResourceListCacheKey(const char *resource, int64_t user_id, int64_t version, int page, int page_size) {
    std::string key = "u:" + std::to_string(user_id) + ":" + resource + "s:v" + std::to_string(version);
    if (page_size > 0)
        key += ":p" + std::to_string(page) + ":ps" + std::to_string(page_size);
    return key;
}
inline std::string ResourceCacheKey(const char *resource, int64_t user_id, int64_t id) {
    return "u:" + std::to_string(user_id) + ":" + resource + ":" + std::to_string(id);
}
inline std::string ResourceLockKey(const char *resource, int64_t user_id, int64_t id) {
    return "lock:u:" + std::to_string(user_id) + ":" + resource + ":" + std::to_string(id);
}

// InvalidateCaches — 通用的 cache invalidation outbox 写入
// 每个 key 作为 payload 插入一条 "cache:invalidate" outbox 条目
//  notify-service 轮询后推 Redis Pub/Sub → 所有进程 L1CacheInvalidator 处理
template <typename DB>
inline void InvalidateCaches(DB *db, int64_t user_id, std::initializer_list<std::string> keys) {
    if (!db)
        return;
    for (auto &key : keys)
        db->InsertOutbox(user_id, "cache:invalidate", key);
}
