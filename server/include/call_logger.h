#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <chrono>
#include "rpc_json.h"

using json = rpc_json::Value;

class RedisClient;

struct CallEntry {
    int id;
    std::string timestamp;
    std::string username;
    std::string service;
    std::string method;
    json params;
    json result;
    bool success;
    int64_t duration_us;
};

class CallLogger {
public:
    CallLogger(size_t max_entries = 1000, RedisClient* redis = nullptr);

    void Log(const std::string& username, const std::string& service,
             const std::string& method, const json& params, const json& result,
             bool success, int64_t duration_us);
    std::vector<CallEntry> GetHistory(int limit = 50, int offset = 0,
                                      const std::string& service_filter = "",
                                      const std::string& method_filter = "") const;
    size_t TotalCount() const;

private:
    mutable std::mutex mtx_;
    std::deque<CallEntry> entries_;
    size_t max_entries_;
    int next_id_ = 1;
    RedisClient* redis_ = nullptr;

    std::string NowString() const;
    std::string SerializeEntry(const CallEntry& entry) const;
};
