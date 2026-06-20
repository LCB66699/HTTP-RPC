#pragma once
#include "shared/cache_helpers.h"

// 向后兼容 thin wrapper �?内部委托�?cache_helpers.h
inline std::string SheetVersionKey(int64_t user_id)     { return ResourceVersionKey("sheet", user_id); }
inline std::string SheetListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    return ResourceListCacheKey("sheet", user_id, version, page, page_size);
}
inline std::string SheetCacheKey(int64_t user_id, int64_t sheet_id)   { return ResourceCacheKey("sheet", user_id, sheet_id); }
inline std::string SheetLockKey(int64_t user_id, int64_t sheet_id)     { return ResourceLockKey("sheet", user_id, sheet_id); }
