#pragma once
#include "cache_helpers.h"

// 向后兼容 thin wrapper — 内部委托到 cache_helpers.h
inline std::string FileVersionKey(int64_t user_id)     { return ResourceVersionKey("file", user_id); }
inline std::string FileListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    return ResourceListCacheKey("file", user_id, version, page, page_size);
}
inline std::string FileCacheKey(int64_t user_id, int64_t file_id)   { return ResourceCacheKey("file", user_id, file_id); }
inline std::string FileLockKey(int64_t user_id, int64_t file_id)     { return ResourceLockKey("file", user_id, file_id); }
