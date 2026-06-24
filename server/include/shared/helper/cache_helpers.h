#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>

// ======== 通用缓存 key 生成 ========

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

// ======== File 快捷函数 ========

inline std::string FileVersionKey(int64_t user_id)     { return ResourceVersionKey("file", user_id); }
inline std::string FileListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    return ResourceListCacheKey("file", user_id, version, page, page_size);
}
inline std::string FileCacheKey(int64_t user_id, int64_t file_id)   { return ResourceCacheKey("file", user_id, file_id); }
inline std::string FileLockKey(int64_t user_id, int64_t file_id)     { return ResourceLockKey("file", user_id, file_id); }

// ======== Sheet 快捷函数 ========

inline std::string SheetVersionKey(int64_t user_id)     { return ResourceVersionKey("sheet", user_id); }
inline std::string SheetListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    return ResourceListCacheKey("sheet", user_id, version, page, page_size);
}
inline std::string SheetCacheKey(int64_t user_id, int64_t sheet_id)   { return ResourceCacheKey("sheet", user_id, sheet_id); }
inline std::string SheetLockKey(int64_t user_id, int64_t sheet_id)     { return ResourceLockKey("sheet", user_id, sheet_id); }

// ======== Cache invalidation ========

// InvalidateCaches — 通用的 cache invalidation
// 1. L1 同步删除 (writer 自身)
// 2. Redis Pub/Sub 广播 (其他实例, ms 级)
// 3. outbox 兜底 (订阅者离线时的补偿)
// notify-service 轮询后补推 Redis Pub/Sub
class L1Cache;
class IRedisClient;

template <typename DB>
inline void InvalidateCaches(DB *db, int64_t user_id, std::initializer_list<std::string> keys,
                             L1Cache *l1 = nullptr, IRedisClient *redis = nullptr) {
    if (!db)
        return;
    for (auto &key : keys) {
        if (l1)
            l1->Delete(key);                                       // 1. sync L1
        if (redis && redis->IsConnected())
            redis->Publish("cache:invalidate", key);               // 2. sync Pub/Sub
        db->InsertOutbox(user_id, "cache:invalidate", key);        // 3. outbox fallback
    }
}
