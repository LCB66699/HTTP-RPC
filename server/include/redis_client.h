// Redis Cluster client — wraps sw::redis::RedisCluster for auto slot routing,
// MOVED/ASK redirect, topology discovery, connection pooling, and failover.
#pragma once
#include <string>
#include <vector>
#include <memory>

namespace sw { namespace redis { class RedisCluster; } }

class RedisClient {
public:
    // cluster_seeds: "host:port" strings (at least one, preferably 3+)
    RedisClient(const std::vector<std::string>& cluster_seeds,
                const std::string& password = "",
                int pool_size = 4);
    ~RedisClient();

    bool Connect();
    bool IsConnected() const;

    // Call history (list operations)
    bool PushCallEntry(const std::string& json_entry, const std::string& username = "");
    std::vector<std::string> GetCallEntries(int limit, int offset,
                                            const std::string& username = "") const;
    int64_t GetCallCount(const std::string& username = "") const;

    // Cache writes
    bool SetJSON(const std::string& key, const std::string& value, int ttl_seconds);
    bool SetNX(const std::string& key, const std::string& value, int ttl_seconds);
    bool DeleteKey(const std::string& key);
    int64_t Increment(const std::string& key);
    int64_t IncrementWithTTL(const std::string& key, int ttl_seconds);

    // Cache reads
    bool GetJSON(const std::string& key, std::string& value);
    int64_t GetInt(const std::string& key);

    // Health check managed internally by the library — no explicit Start/Stop needed.
    // Kept for backward compatibility (no-ops).
    void StartHealthCheck() {}
    void StopHealthCheck() {}

private:
    std::vector<std::string> cluster_seeds_;
    std::string password_;
    int pool_size_;
    std::unique_ptr<sw::redis::RedisCluster> cluster_;

    static constexpr size_t MAX_HISTORY = 10000;
    static constexpr int CALL_HISTORY_TTL_SECONDS = 30 * 86400;
};
