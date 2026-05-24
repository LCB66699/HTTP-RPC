#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

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
    ~CallLogger();

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

    // 后台 flush 线程 — Redis 网络 I/O 不阻塞业务线程
    std::vector<std::pair<std::string, std::string>> pending_;  // (json, username)
    std::condition_variable cv_;
    std::thread flush_thread_;
    std::atomic<bool> running_{true};

    void FlushLoop();

    std::string NowString() const;
    static std::string SerializeEntry(const CallEntry& entry);
};
