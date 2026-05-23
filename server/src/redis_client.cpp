#include "redis_client.h"
#include <sw/redis++/redis++.h>
#include <cstdio>
#include <chrono>

RedisClient::RedisClient(const std::vector<std::string>& cluster_seeds,
                         const std::string& password, int pool_size)
    : cluster_seeds_(cluster_seeds), password_(password), pool_size_(pool_size) {}

RedisClient::~RedisClient() {
    cluster_.reset();
}

bool RedisClient::Connect() {
    try {
        sw::redis::ConnectionOptions opts;
        // Use first seed as initial connection point
        auto colon = cluster_seeds_[0].find(':');
        opts.host = cluster_seeds_[0].substr(0, colon);
        opts.port = std::stoi(cluster_seeds_[0].substr(colon + 1));
        if (!password_.empty()) {
            opts.password = password_;
        }
        opts.connect_timeout = std::chrono::milliseconds(500);
        opts.socket_timeout = std::chrono::milliseconds(1000);

        sw::redis::ConnectionPoolOptions pool_opts;
        pool_opts.size = pool_size_;
        pool_opts.wait_timeout = std::chrono::milliseconds(100);

        cluster_ = std::make_unique<sw::redis::RedisCluster>(opts, pool_opts);
        printf("[Redis] Cluster connected (seeds: %zu, pool: %d/seed)\n",
               cluster_seeds_.size(), pool_size_);
        return true;
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] Cluster connect failed: %s\n", e.what());
        return false;
    }
}

bool RedisClient::IsConnected() const {
    if (!cluster_) return false;
    try {
        auto val = cluster_->get("__health__");
        return true;
    } catch (...) {
        return false;
    }
}

// ---- call history ----

bool RedisClient::PushCallEntry(const std::string& json_entry, const std::string& username) {
    if (!cluster_) return false;
    try {
        const std::string global_key = "call_history:global";
        cluster_->lpush(global_key, json_entry);
        cluster_->ltrim(global_key, 0, MAX_HISTORY - 1);
        cluster_->expire(global_key, std::chrono::seconds(CALL_HISTORY_TTL_SECONDS));

        if (!username.empty()) {
            const std::string user_key = "call_history:" + username;
            cluster_->lpush(user_key, json_entry);
            cluster_->ltrim(user_key, 0, MAX_HISTORY - 1);
            cluster_->expire(user_key, std::chrono::seconds(CALL_HISTORY_TTL_SECONDS));
        }
        return true;
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] PushCallEntry failed: %s\n", e.what());
        return false;
    }
}

std::vector<std::string> RedisClient::GetCallEntries(int limit, int offset,
                                                      const std::string& username) const {
    if (!cluster_) return {};
    try {
        const std::string key = username.empty() ? "call_history:global"
                                                 : "call_history:" + username;
        std::vector<std::string> entries;
        cluster_->lrange(key, offset, offset + limit - 1, std::back_inserter(entries));
        return entries;
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] GetCallEntries failed: %s\n", e.what());
        return {};
    }
}

int64_t RedisClient::GetCallCount(const std::string& username) const {
    if (!cluster_) return 0;
    try {
        const std::string key = username.empty() ? "call_history:global"
                                                 : "call_history:" + username;
        return cluster_->llen(key);
    } catch (const sw::redis::Error& e) {
        return 0;
    }
}

// ---- cache writes ----

bool RedisClient::SetJSON(const std::string& key, const std::string& value, int ttl) {
    if (!cluster_) return false;
    try {
        cluster_->setex(key, ttl, value);
        return true;
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] SetJSON(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

bool RedisClient::SetNX(const std::string& key, const std::string& value, int ttl) {
    if (!cluster_) return false;
    try {
        return cluster_->set(key, value, std::chrono::seconds(ttl),
                             sw::redis::UpdateType::NOT_EXIST);
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] SetNX(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

bool RedisClient::DeleteKey(const std::string& key) {
    if (!cluster_) return false;
    try {
        cluster_->del(key);
        return true;
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] DeleteKey(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

int64_t RedisClient::Increment(const std::string& key) {
    if (!cluster_) return 0;
    try {
        return cluster_->incr(key);
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] Increment(%s) failed: %s\n", key.c_str(), e.what());
        return 0;
    }
}

int64_t RedisClient::IncrementWithTTL(const std::string& key, int ttl) {
    if (!cluster_) return 0;
    try {
        long long val = cluster_->eval<long long>(
            "local c = redis.call('INCR', KEYS[1]) "
            "if c == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
            "return c",
            {key}, {std::to_string(ttl)});
        return val;
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] IncrementWithTTL(%s) failed: %s\n", key.c_str(), e.what());
        return 0;
    }
}

// ---- cache reads ----

bool RedisClient::GetJSON(const std::string& key, std::string& value) {
    if (!cluster_) return false;
    try {
        auto val = cluster_->get(key);
        if (val) { value = *val; return true; }
        return false;
    } catch (const sw::redis::Error& e) {
        fprintf(stderr, "[Redis] GetJSON(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

int64_t RedisClient::GetInt(const std::string& key) {
    if (!cluster_) return 0;
    try {
        auto val = cluster_->get(key);
        if (val) return std::stoll(*val);
        return 0;
    } catch (const sw::redis::Error&) {
        return 0;
    }
}
