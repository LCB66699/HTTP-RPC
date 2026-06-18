// Redis Cluster client — wraps sw::redis::RedisCluster for auto slot routing,
// MOVED/ASK redirect, topology discovery, connection pooling, and failover.
#pragma once
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "service_interfaces.h"

namespace sw {
namespace redis {
class RedisCluster;
}
}  // namespace sw

class RedisClient : public IRedisClient {
   public:
    // cluster_seeds: "host:port" strings (at least one, preferably 3+)
    RedisClient(const std::vector<std::string> &cluster_seeds, const std::string &password = "", int pool_size = 4);
    ~RedisClient();

    bool Connect();
    bool IsConnected() const;

    // Call history (list operations)
    bool PushCallEntry(const std::string &json_entry, const std::string &username = "");
    std::vector<std::string> GetCallEntries(int limit, int offset, const std::string &username = "") const;
    int64_t GetCallCount(const std::string &username = "") const;
    std::vector<std::string> GetHistoryUsers() const;

    // Cache writes
    bool SetJSON(const std::string &key, const std::string &value, int ttl_seconds);
    bool SetNX(const std::string &key, const std::string &value, int ttl_seconds);
    bool DeleteKey(const std::string &key);
    bool ExpireKey(const std::string &key, int ttl_seconds);
    int64_t Increment(const std::string &key);
    int64_t IncrementWithTTL(const std::string &key, int ttl_seconds);
    bool HSetJSON(const std::string &key, const std::string &field, const std::string &value);
    bool HSetJSON(const std::string &key, const std::string &field, const std::string &value, int ttl_seconds);

    // Cache reads
    bool GetJSON(const std::string &key, std::string &value);
    int64_t GetInt(const std::string &key);
    std::unordered_map<std::string, std::string> HGetAll(const std::string &key) const;

    // Pub/Sub — Publish is cluster-safe; Subscribe needs standalone connection
    bool Publish(const std::string &channel, const std::string &message);

    // Subscribe helper: returns a standalone subscriber to one cluster node.
    // Caller is responsible for lifecycle. Callback runs in a dedicated thread.
    using SubCallback = std::function<void(const std::string &channel, const std::string &msg)>;
    void SubscribeStandalone(const std::string &channel, SubCallback cb);

    // Health check managed internally by the library — no explicit Start/Stop
    // needed. Kept for backward compatibility (no-ops).
    void StartHealthCheck() {}
    void StopHealthCheck() {}

    // 给 TTL 加随机偏移，防止缓存雪崩。jitter 为偏移上限（秒）。
    static int JitteredTTL(int base_ttl, int jitter) { return base_ttl + (std::rand() % (jitter + 1)); }

   private:
    std::vector<std::string> cluster_seeds_;
    std::string password_;
    int pool_size_;
    std::unique_ptr<sw::redis::RedisCluster> cluster_;

    static constexpr size_t MAX_HISTORY = 10000;
    static constexpr int CALL_HISTORY_TTL_SECONDS = 30 * 86400;
};
