#pragma once
#include <cstdint>
#include <string>

// Redis cache key helpers for spreadsheet service.
// Extracted from spreadsheet_service_impl.cpp for testability.
inline std::string SheetVersionKey(int64_t user_id) {
    return "u:" + std::to_string(user_id) + ":sheets:version";
}
inline std::string SheetListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    std::string key = "u:" + std::to_string(user_id) + ":sheets:v" + std::to_string(version);
    if (page_size > 0)
        key += ":p" + std::to_string(page) + ":ps" + std::to_string(page_size);
    return key;
}
inline std::string SheetCacheKey(int64_t user_id, int64_t sheet_id) {
    return "u:" + std::to_string(user_id) + ":sheet:" + std::to_string(sheet_id);
}
inline std::string SheetLockKey(int64_t user_id, int64_t sheet_id) {
    return "lock:u:" + std::to_string(user_id) + ":sheet:" + std::to_string(sheet_id);
}
