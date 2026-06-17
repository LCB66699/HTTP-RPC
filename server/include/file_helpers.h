#pragma once
#include <cstdint>
#include <string>

// Redis cache key helpers for file service.
// Extracted from file_service_impl.cpp for testability.
inline std::string FileVersionKey(int64_t user_id) {
    return "u:" + std::to_string(user_id) + ":files:version";
}
inline std::string FileListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    std::string key = "u:" + std::to_string(user_id) + ":files:v" + std::to_string(version);
    if (page_size > 0)
        key += ":p" + std::to_string(page) + ":ps" + std::to_string(page_size);
    return key;
}
inline std::string FileCacheKey(int64_t user_id, int64_t file_id) {
    return "u:" + std::to_string(user_id) + ":file:" + std::to_string(file_id);
}
inline std::string FileLockKey(int64_t user_id, int64_t file_id) {
    return "lock:u:" + std::to_string(user_id) + ":file:" + std::to_string(file_id);
}
