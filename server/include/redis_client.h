// Redis 读写分离: 主库连接池写 + 从库连接池读 (DNS 别名轮询)
// Sentinel 故障转移: Master 宕机自动发现新 Master 重连
// hiredis C 库 RESP protocol over TCP 6379
#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <hiredis/hiredis.h>

struct RedisPoolConn {
    redisContext* ctx = nullptr;
    std::mutex mtx;
};

class RedisClient {
public:
    RedisClient(const std::string& master_host, int master_port,
                const std::string& slave_host, int slave_port,
                const std::string& password = "",
                const std::string& sentinel_host = "",
                int sentinel_port = 26379,
                const std::string& master_name = "redis-master",
                int pool_size = 4);
    ~RedisClient();

    bool Connect();
    bool IsConnected() const;
    void Disconnect();

    // Call history
    // Key convention: call_history:{username} for per-user, call_history:global for aggregate.
    // username="" pushes/reads only the global list; non-empty username pushes to BOTH lists.
    bool PushCallEntry(const std::string& json_entry, const std::string& username = "");
    std::vector<std::string> GetCallEntries(int limit, int offset,
                                            const std::string& username = "") const;
    int64_t GetCallCount(const std::string& username = "") const;

    // Cache: write methods → master pool (auto failover via Sentinel)
    bool SetJSON(const std::string& key, const std::string& value, int ttl_seconds);
    // SET key value NX EX ttl — returns true only if key did not exist (acquired)
    bool SetNX(const std::string& key, const std::string& value, int ttl_seconds);
    bool DeleteKey(const std::string& key);
    int64_t Increment(const std::string& key);
    // 原子 INCR + 首次创建时设 TTL（Lua EVAL，防 EXPIRE 竞态）
    int64_t IncrementWithTTL(const std::string& key, int ttl_seconds);

    // Cache: read methods → slave pool (fallback to master pool)
    bool GetJSON(const std::string& key, std::string& value);
    int64_t GetInt(const std::string& key);

    // 健康检查：后台线程每 30s PING 连接池，自动重建死连接
    void StartHealthCheck();
    void StopHealthCheck();

private:
    std::string master_host_, slave_host_, password_;
    int master_port_, slave_port_;

    // Sentinel 配置
    std::string sentinel_host_;
    int sentinel_port_;
    std::string master_name_;

    int pool_size_;

    // 连接池 — per-conn mutex + 无锁 round-robin 分发
    std::vector<std::unique_ptr<RedisPoolConn>> master_pool_;
    std::vector<std::unique_ptr<RedisPoolConn>> slave_pool_;
    mutable std::atomic<size_t> master_idx_{0};
    mutable std::atomic<size_t> slave_idx_{0};

    static constexpr size_t MAX_HISTORY = 10000;

    std::thread health_check_;
    std::atomic<bool> health_running_{false};

    bool ConnectOne(RedisPoolConn& conn, const std::string& host, int port);
    bool AuthIfNeeded(redisContext* ctx);
    bool ReconnectOne(RedisPoolConn& conn);
    bool QueryMasterFromSentinel(std::string& host, int& port);
};
