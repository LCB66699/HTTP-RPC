#include "redis_client.h"

#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdio>

RedisClient::RedisClient(const std::vector<std::string> &cluster_seeds, const std::string &password, int pool_size)
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
        printf("[Redis] Cluster connected (seeds: %zu, pool: %d/seed)\n", cluster_seeds_.size(), pool_size_);
        return true;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] Cluster connect failed: %s\n", e.what());
        return false;
    }
}

bool RedisClient::IsConnected() const {
    if (!cluster_)
        return false;
    try {
        auto val = cluster_->get("__health__");
        return true;
    } catch (...) {
        return false;
    }
}

// ---- call history ----

bool RedisClient::PushCallEntry(const std::string &json_entry, const std::string &username) {
    if (!cluster_)
        return false;
    try {
        if (!username.empty()) {
            const std::string user_key = "call_history:" + username;
            cluster_->lpush(user_key, json_entry);
            cluster_->ltrim(user_key, 0, MAX_HISTORY - 1);
            cluster_->expire(user_key, std::chrono::seconds(CALL_HISTORY_TTL_SECONDS));
            cluster_->sadd("call_history:users", username);
            cluster_->expire("call_history:users", std::chrono::seconds(CALL_HISTORY_TTL_SECONDS));
        }
        return true;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] PushCallEntry failed: %s\n", e.what());
        return false;
    }
}

std::vector<std::string> RedisClient::GetCallEntries(int limit, int offset, const std::string &username) const {
    if (!cluster_)
        return {};
    try {
        const std::string key = "call_history:" + username;
        std::vector<std::string> entries;
        cluster_->lrange(key, offset, offset + limit - 1, std::back_inserter(entries));
        return entries;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] GetCallEntries failed: %s\n", e.what());
        return {};
    }
}

int64_t RedisClient::GetCallCount(const std::string &username) const {
    if (!cluster_)
        return 0;
    try {
        const std::string key = "call_history:" + username;
        return cluster_->llen(key);
    } catch (const sw::redis::Error &e) {
        return 0;
    }
}

std::vector<std::string> RedisClient::GetHistoryUsers() const {
    if (!cluster_)
        return {};
    try {
        std::vector<std::string> users;
        cluster_->smembers("call_history:users", std::back_inserter(users));
        std::sort(users.begin(), users.end());
        return users;
    } catch (const sw::redis::Error &e) {
        return {};
    }
}

// ---- cache writes ----

bool RedisClient::SetJSON(const std::string &key, const std::string &value, int ttl) {
    if (!cluster_)
        return false;
    try {
        cluster_->setex(key, ttl, value);
        return true;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] SetJSON(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

bool RedisClient::SetNX(const std::string &key, const std::string &value, int ttl) {
    if (!cluster_)
        return false;
    try {
        return cluster_->set(key, value, std::chrono::seconds(ttl), sw::redis::UpdateType::NOT_EXIST);
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] SetNX(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

bool RedisClient::DeleteKey(const std::string &key) {
    if (!cluster_)
        return false;
    try {
        cluster_->del(key);
        return true;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] DeleteKey(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

int64_t RedisClient::Increment(const std::string &key) {
    if (!cluster_)
        return 0;
    try {
        auto val = cluster_->incr(key);
        cluster_->expire(key, std::chrono::seconds(7 * 86400));  // 7 day TTL
        return val;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] Increment(%s) failed: %s\n", key.c_str(), e.what());
        return 0;
    }
}

int64_t RedisClient::IncrementWithTTL(const std::string &key, int ttl) {
    if (!cluster_)
        return 0;
    try {
        long long val = cluster_->eval<long long>(
            "local c = redis.call('INCR', KEYS[1]) "
            "if c == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
            "return c",
            {key}, {std::to_string(ttl)});
        return val;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] IncrementWithTTL(%s) failed: %s\n", key.c_str(), e.what());
        return 0;
    }
}

bool RedisClient::ExpireKey(const std::string &key, int ttl) {
    if (!cluster_)
        return false;
    try {
        cluster_->expire(key, std::chrono::seconds(ttl));
        return true;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] ExpireKey(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

bool RedisClient::HSetJSON(const std::string &key, const std::string &field, const std::string &value) {
    if (!cluster_)
        return false;
    try {
        cluster_->hset(key, field, value);
        return true;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] HSetJSON(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

bool RedisClient::HSetJSON(const std::string &key, const std::string &field, const std::string &value, int ttl) {
    if (!HSetJSON(key, field, value))
        return false;
    return ExpireKey(key, ttl);
}

std::unordered_map<std::string, std::string> RedisClient::HGetAll(const std::string &key) const {
    if (!cluster_)
        return {};
    try {
        std::unordered_map<std::string, std::string> result;
        cluster_->hgetall(key, std::inserter(result, result.begin()));
        return result;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] HGetAll(%s) failed: %s\n", key.c_str(), e.what());
        return {};
    }
}

// ---- cache reads ----

bool RedisClient::GetJSON(const std::string &key, std::string &value) {
    if (!cluster_)
        return false;
    try {
        auto val = cluster_->get(key);
        if (val) {
            value = *val;
            return true;
        }
        return false;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] GetJSON(%s) failed: %s\n", key.c_str(), e.what());
        return false;
    }
}

int64_t RedisClient::GetInt(const std::string &key) {
    if (!cluster_)
        return 0;
    try {
        auto val = cluster_->get(key);
        if (val)
            return std::stoll(*val);
        return 0;
    } catch (const sw::redis::Error &) {
        return 0;
    }
}

// ---- Pub/Sub ----

bool RedisClient::Publish(const std::string &channel, const std::string &message) {
    if (!cluster_)
        return false;
    try {
        cluster_->publish(channel, message);
        return true;
    } catch (const sw::redis::Error &e) {
        fprintf(stderr, "[Redis] Publish(%s) failed: %s\n", channel.c_str(), e.what());
        return false;
    }
}

void RedisClient::SubscribeStandalone(const std::string &channel, SubCallback cb) {
    if (cluster_seeds_.empty())
        return;
    std::thread([this, channel, cb = std::move(cb)]() {
        try {
            auto colon = cluster_seeds_[0].find(':');
            sw::redis::ConnectionOptions opts;
            opts.host = cluster_seeds_[0].substr(0, colon);
            opts.port = std::stoi(cluster_seeds_[0].substr(colon + 1));
            if (!password_.empty())
                opts.password = password_;
            opts.connect_timeout = std::chrono::milliseconds(500);
            opts.socket_timeout = std::chrono::milliseconds(0);  // blocking

            auto standalone = std::make_unique<sw::redis::Redis>(opts);
            auto sub = std::make_unique<sw::redis::Subscriber>(standalone->subscriber());
            sub->on_message([cb](std::string ch, std::string msg) { cb(ch, msg); });
            sub->subscribe(channel);
            // blocks until subscriber is destroyed
            while (true) {
                try {
                    sub->consume();
                } catch (const sw::redis::TimeoutError &) {
                    continue;
                } catch (const std::exception &e) {
                    fprintf(stderr, "[Redis] Subscriber error: %s, reconnecting in 1s\n", e.what());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    try {
                        sub->subscribe(channel);
                    } catch (...) {
                        break;
                    }
                }
            }
        } catch (const std::exception &e) {
            fprintf(stderr, "[Redis] SubscribeStandalone failed: %s\n", e.what());
        }
    }).detach();
}
