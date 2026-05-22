#include "redis_client.h"
#include <cstdio>
#include <cstdarg>

RedisClient::RedisClient(const std::string& master_host, int master_port,
                         const std::string& slave_host, int slave_port,
                         const std::string& password,
                         const std::string& sentinel_host,
                         int sentinel_port,
                         const std::string& master_name,
                         int pool_size)
    : master_host_(master_host), slave_host_(slave_host),
      password_(password),
      master_port_(master_port), slave_port_(slave_port),
      sentinel_host_(sentinel_host), sentinel_port_(sentinel_port),
      master_name_(master_name), pool_size_(pool_size) {}

RedisClient::~RedisClient() { StopHealthCheck(); Disconnect(); }

bool RedisClient::AuthIfNeeded(redisContext* ctx) {
    if (!ctx || password_.empty()) return true;
    redisReply* reply = (redisReply*)redisCommand(ctx, "AUTH %s", password_.c_str());
    bool ok = reply && reply->type == REDIS_REPLY_STATUS;
    if (reply) freeReplyObject(reply);
    return ok;
}

// ---- connection pool helpers ----

bool RedisClient::ConnectOne(RedisPoolConn& conn, const std::string& host, int port) {
    conn.ctx = redisConnect(host.c_str(), port);
    if (!conn.ctx || conn.ctx->err) {
        fprintf(stderr, "[Redis:pool] connect %s:%d failed: %s\n",
                host.c_str(), port, conn.ctx ? conn.ctx->errstr : "no ctx");
        if (conn.ctx) { redisFree(conn.ctx); conn.ctx = nullptr; }
        return false;
    }
    if (!AuthIfNeeded(conn.ctx)) {
        fprintf(stderr, "[Redis:pool] AUTH failed %s:%d\n", host.c_str(), port);
        redisFree(conn.ctx);
        conn.ctx = nullptr;
        return false;
    }
    return true;
}

bool RedisClient::Connect() {
    for (int i = 0; i < pool_size_; ++i) {
        auto conn = std::make_unique<RedisPoolConn>();
        if (ConnectOne(*conn, master_host_, master_port_))
            printf("[Redis:pool] master conn %d/%d connected\n", i + 1, pool_size_);
        master_pool_.push_back(std::move(conn));
    }

    for (int i = 0; i < pool_size_; ++i) {
        auto conn = std::make_unique<RedisPoolConn>();
        if (ConnectOne(*conn, slave_host_, slave_port_))
            printf("[Redis:pool] slave  conn %d/%d connected\n", i + 1, pool_size_);
        // slave failed → ctx stays nullptr, reads will fallback to master
        slave_pool_.push_back(std::move(conn));
    }

    for (auto& c : master_pool_) {
        if (c->ctx) return true;
    }
    fprintf(stderr, "[Redis:pool] no master connection available\n");
    return false;
}

bool RedisClient::IsConnected() const {
    for (auto& c : master_pool_)
        if (c->ctx) return true;
    return false;
}

void RedisClient::Disconnect() {
    for (auto& c : master_pool_) {
        if (c->ctx) { redisFree(c->ctx); c->ctx = nullptr; }
    }
    master_pool_.clear();
    for (auto& c : slave_pool_) {
        if (c->ctx) { redisFree(c->ctx); c->ctx = nullptr; }
    }
    slave_pool_.clear();
}

// ---- Sentinel ----

bool RedisClient::QueryMasterFromSentinel(std::string& host, int& port) {
    if (sentinel_host_.empty()) return false;

    redisContext* ctx = redisConnect(sentinel_host_.c_str(), sentinel_port_);
    if (!ctx || ctx->err) {
        if (ctx) redisFree(ctx);
        ctx = redisConnect(sentinel_host_.c_str(), sentinel_port_);
        if (!ctx || ctx->err) {
            fprintf(stderr, "[Redis:sentinel] connect failed: %s\n",
                    ctx ? ctx->errstr : "no ctx");
            if (ctx) redisFree(ctx);
            return false;
        }
    }

    if (!password_.empty()) {
        redisReply* auth = (redisReply*)redisCommand(ctx, "AUTH %s", password_.c_str());
        if (auth) freeReplyObject(auth);
    }

    redisReply* reply = (redisReply*)redisCommand(
        ctx, "SENTINEL get-master-addr-by-name %s", master_name_.c_str());
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
        fprintf(stderr, "[Redis:sentinel] get-master-addr-by-name failed\n");
        if (reply) freeReplyObject(reply);
        redisFree(ctx);
        return false;
    }
    host = reply->element[0]->str;
    port = std::stoi(reply->element[1]->str);
    freeReplyObject(reply);
    redisFree(ctx);
    return true;
}

bool RedisClient::ReconnectOne(RedisPoolConn& conn) {
    // 1) Try direct reconnect to original address
    conn.ctx = redisConnect(master_host_.c_str(), master_port_);
    if (conn.ctx && !conn.ctx->err && AuthIfNeeded(conn.ctx)) {
        printf("[Redis:pool] reconnected %s:%d\n", master_host_.c_str(), master_port_);
        return true;
    }
    if (conn.ctx) { redisFree(conn.ctx); conn.ctx = nullptr; }

    // 2) Sentinel failover — discover new master
    std::string new_host;
    int new_port;
    if (!QueryMasterFromSentinel(new_host, new_port)) {
        fprintf(stderr, "[Redis:pool] sentinel query failed, cannot recover\n");
        return false;
    }

    if (new_host == master_host_ && new_port == master_port_) {
        // Sentinel confirms same master — retry once more
        conn.ctx = redisConnect(master_host_.c_str(), master_port_);
        if (conn.ctx && !conn.ctx->err && AuthIfNeeded(conn.ctx)) {
            printf("[Redis:pool] reconnected %s:%d (sentinel confirmed)\n",
                   master_host_.c_str(), master_port_);
            return true;
        }
        if (conn.ctx) { redisFree(conn.ctx); conn.ctx = nullptr; }
        return false;
    }

    // Sentinel returned a different master → failover occurred
    master_host_ = new_host;
    master_port_ = new_port;
    conn.ctx = redisConnect(master_host_.c_str(), master_port_);
    if (!conn.ctx || conn.ctx->err || !AuthIfNeeded(conn.ctx)) {
        fprintf(stderr, "[Redis:pool] failover reconnect to %s:%d failed\n",
                master_host_.c_str(), master_port_);
        if (conn.ctx) { redisFree(conn.ctx); conn.ctx = nullptr; }
        return false;
    }

    printf("[Redis:pool] FAILOVER — new master %s:%d\n", master_host_.c_str(), master_port_);
    return true;
}

// ---- call history (LPUSH → master pool, LRANGE/LLEN → slave pool with fallback) ----

// Key naming: call_history:global (aggregate) + call_history:{username} (per-user).
// Both lists are capped at MAX_HISTORY entries and given a 30-day rolling TTL.
static constexpr int CALL_HISTORY_TTL_SECONDS = 30 * 86400;  // 30 days

static bool pushToKey(redisContext* ctx, const std::string& key,
                      const std::string& json_entry, size_t max_entries) {
    redisReply* reply = (redisReply*)redisCommand(ctx, "LPUSH %s %b",
                                                   key.c_str(),
                                                   json_entry.data(), json_entry.size());
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    if (ok) {
        reply = (redisReply*)redisCommand(ctx, "LTRIM %s 0 %zu", key.c_str(), max_entries - 1);
        if (reply) freeReplyObject(reply);
        reply = (redisReply*)redisCommand(ctx, "EXPIRE %s %d", key.c_str(), CALL_HISTORY_TTL_SECONDS);
        if (reply) freeReplyObject(reply);
    }
    return ok;
}

bool RedisClient::PushCallEntry(const std::string& json_entry, const std::string& username) {
    if (master_pool_.empty()) return false;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);

    if (!conn->ctx && !ReconnectOne(*conn)) return false;

    const std::string global_key = "call_history:global";
    bool ok = pushToKey(conn->ctx, global_key, json_entry, MAX_HISTORY);
    if (!ok) {
        redisFree(conn->ctx); conn->ctx = nullptr;
        if (!ReconnectOne(*conn)) return false;
        ok = pushToKey(conn->ctx, global_key, json_entry, MAX_HISTORY);
    }

    // Also maintain per-user list when username is provided
    if (ok && !username.empty()) {
        const std::string user_key = "call_history:" + username;
        pushToKey(conn->ctx, user_key, json_entry, MAX_HISTORY);
    }
    return ok;
}

std::vector<std::string> RedisClient::GetCallEntries(int limit, int offset,
                                                      const std::string& username) const {
    const std::string key = username.empty() ? "call_history:global"
                                             : "call_history:" + username;
    int end = offset + limit - 1;

    // Try slave pool first
    if (!slave_pool_.empty()) {
        size_t idx = slave_idx_.fetch_add(1, std::memory_order_relaxed) % slave_pool_.size();
        auto& conn = slave_pool_[idx];
        std::lock_guard<std::mutex> lock(conn->mtx);
        if (conn->ctx) {
            redisReply* reply = (redisReply*)redisCommand(conn->ctx, "LRANGE %s %d %d",
                                                           key.c_str(), offset, end);
            if (reply && reply->type == REDIS_REPLY_ARRAY) {
                std::vector<std::string> entries;
                for (size_t i = 0; i < reply->elements; ++i)
                    if (reply->element[i]->type == REDIS_REPLY_STRING)
                        entries.emplace_back(reply->element[i]->str, reply->element[i]->len);
                freeReplyObject(reply);
                return entries;
            }
            if (reply) freeReplyObject(reply);
        }
    }
    // Fallback to master pool
    if (master_pool_.empty()) return {};
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);
    if (!conn->ctx) return {};

    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "LRANGE %s %d %d",
                                                   key.c_str(), offset, end);
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return {};
    }
    std::vector<std::string> entries;
    for (size_t i = 0; i < reply->elements; ++i)
        if (reply->element[i]->type == REDIS_REPLY_STRING)
            entries.emplace_back(reply->element[i]->str, reply->element[i]->len);
    freeReplyObject(reply);
    return entries;
}

int64_t RedisClient::GetCallCount(const std::string& username) const {
    const std::string key = username.empty() ? "call_history:global"
                                             : "call_history:" + username;

    // Try slave pool first
    if (!slave_pool_.empty()) {
        size_t idx = slave_idx_.fetch_add(1, std::memory_order_relaxed) % slave_pool_.size();
        auto& conn = slave_pool_[idx];
        std::lock_guard<std::mutex> lock(conn->mtx);
        if (conn->ctx) {
            redisReply* reply = (redisReply*)redisCommand(conn->ctx, "LLEN %s", key.c_str());
            if (reply && reply->type == REDIS_REPLY_INTEGER) {
                int64_t count = reply->integer;
                freeReplyObject(reply);
                return count;
            }
            if (reply) freeReplyObject(reply);
        }
    }
    // Fallback to master pool
    if (master_pool_.empty()) return 0;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);
    if (!conn->ctx) return 0;

    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "LLEN %s", key.c_str());
    if (!reply || reply->type != REDIS_REPLY_INTEGER) {
        if (reply) freeReplyObject(reply);
        return 0;
    }
    int64_t count = reply->integer;
    freeReplyObject(reply);
    return count;
}

// ---- cache writes → master pool (with Sentinel failover) ----

bool RedisClient::SetJSON(const std::string& key, const std::string& value, int ttl) {
    if (master_pool_.empty()) return false;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);

    if (!conn->ctx && !ReconnectOne(*conn)) return false;

    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "SETEX %s %d %b",
                                                   key.c_str(), ttl, value.data(), value.size());
    if (!reply) {
        redisFree(conn->ctx); conn->ctx = nullptr;
        if (!ReconnectOne(*conn)) return false;
        reply = (redisReply*)redisCommand(conn->ctx, "SETEX %s %d %b",
                                           key.c_str(), ttl, value.data(), value.size());
        if (!reply) return false;
    }
    bool ok = (reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::SetNX(const std::string& key, const std::string& value, int ttl) {
    if (master_pool_.empty()) return false;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);

    if (!conn->ctx && !ReconnectOne(*conn)) return false;

    // SET key value NX EX ttl — atomic: only set if not exists, with expiry
    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "SET %s %b NX EX %d",
                                                   key.c_str(), value.data(), value.size(), ttl);
    if (!reply) {
        redisFree(conn->ctx); conn->ctx = nullptr;
        if (!ReconnectOne(*conn)) return false;
        reply = (redisReply*)redisCommand(conn->ctx, "SET %s %b NX EX %d",
                                           key.c_str(), value.data(), value.size(), ttl);
        if (!reply) return false;
    }
    // Redis returns bulk string "OK" if set, nil if key already existed
    bool acquired = (reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);
    return acquired;
}

bool RedisClient::DeleteKey(const std::string& key) {
    if (master_pool_.empty()) return false;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);

    if (!conn->ctx && !ReconnectOne(*conn)) return false;

    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "DEL %s", key.c_str());
    if (!reply) {
        redisFree(conn->ctx); conn->ctx = nullptr;
        if (!ReconnectOne(*conn)) return false;
        reply = (redisReply*)redisCommand(conn->ctx, "DEL %s", key.c_str());
        if (!reply) return false;
    }
    freeReplyObject(reply);
    return true;
}

int64_t RedisClient::Increment(const std::string& key) {
    if (master_pool_.empty()) return 0;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);

    if (!conn->ctx && !ReconnectOne(*conn)) return 0;

    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "INCR %s", key.c_str());
    if (!reply) {
        redisFree(conn->ctx); conn->ctx = nullptr;
        if (!ReconnectOne(*conn)) return 0;
        reply = (redisReply*)redisCommand(conn->ctx, "INCR %s", key.c_str());
        if (!reply || reply->type != REDIS_REPLY_INTEGER) {
            if (reply) freeReplyObject(reply);
            return 0;
        }
    }
    int64_t val = reply->integer;
    freeReplyObject(reply);
    return val;
}

int64_t RedisClient::IncrementWithTTL(const std::string& key, int ttl) {
    if (master_pool_.empty()) return 0;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);

    if (!conn->ctx && !ReconnectOne(*conn)) return 0;

    // Lua: 原子 INCR + 首次创建时设 TTL
    static const char* kScript =
        "local c = redis.call('INCR', KEYS[1]) "
        "if c == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
        "return c";
    redisReply* reply = (redisReply*)redisCommand(conn->ctx,
        "EVAL %s 1 %s %d", kScript, key.c_str(), ttl);
    if (!reply) {
        redisFree(conn->ctx); conn->ctx = nullptr;
        if (!ReconnectOne(*conn)) return 0;
        reply = (redisReply*)redisCommand(conn->ctx,
            "EVAL %s 1 %s %d", kScript, key.c_str(), ttl);
        if (!reply || reply->type != REDIS_REPLY_INTEGER) {
            if (reply) freeReplyObject(reply);
            return 0;
        }
    }
    int64_t val = reply->integer;
    freeReplyObject(reply);
    return val;
}

// ---- cache reads → slave pool (fallback to master pool) ----

bool RedisClient::GetJSON(const std::string& key, std::string& value) {
    // Try slave pool first
    if (!slave_pool_.empty()) {
        size_t idx = slave_idx_.fetch_add(1, std::memory_order_relaxed) % slave_pool_.size();
        auto& conn = slave_pool_[idx];
        std::lock_guard<std::mutex> lock(conn->mtx);
        if (conn->ctx) {
            redisReply* reply = (redisReply*)redisCommand(conn->ctx, "GET %s", key.c_str());
            if (reply) {
                if (reply->type == REDIS_REPLY_STRING) {
                    value.assign(reply->str, reply->len);
                    freeReplyObject(reply);
                    return true;
                }
                freeReplyObject(reply);
                return false;
            }
        }
    }
    // Fallback to master pool
    if (master_pool_.empty()) return false;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);
    if (!conn->ctx) return false;

    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "GET %s", key.c_str());
    if (!reply) return false;
    if (reply->type == REDIS_REPLY_STRING) {
        value.assign(reply->str, reply->len);
        freeReplyObject(reply);
        return true;
    }
    freeReplyObject(reply);
    return false;
}

int64_t RedisClient::GetInt(const std::string& key) {
    // Try slave pool first
    if (!slave_pool_.empty()) {
        size_t idx = slave_idx_.fetch_add(1, std::memory_order_relaxed) % slave_pool_.size();
        auto& conn = slave_pool_[idx];
        std::lock_guard<std::mutex> lock(conn->mtx);
        if (conn->ctx) {
            redisReply* reply = (redisReply*)redisCommand(conn->ctx, "GET %s", key.c_str());
            if (reply && reply->type == REDIS_REPLY_STRING) {
                int64_t val = std::stoll(reply->str);
                freeReplyObject(reply);
                return val;
            }
            if (reply) freeReplyObject(reply);
            return 0;
        }
    }
    // Fallback to master pool
    if (master_pool_.empty()) return 0;
    size_t idx = master_idx_.fetch_add(1, std::memory_order_relaxed) % master_pool_.size();
    auto& conn = master_pool_[idx];
    std::lock_guard<std::mutex> lock(conn->mtx);
    if (!conn->ctx) return 0;

    redisReply* reply = (redisReply*)redisCommand(conn->ctx, "GET %s", key.c_str());
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        return 0;
    }
    int64_t val = std::stoll(reply->str);
    freeReplyObject(reply);
    return val;
}

// ---- 连接池健康检查 ----

void RedisClient::StartHealthCheck() {
    health_running_ = true;
    health_check_ = std::thread([this] {
        while (health_running_) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!health_running_) break;

            for (size_t i = 0; i < master_pool_.size(); ++i) {
                auto& c = master_pool_[i];
                std::lock_guard<std::mutex> lock(c->mtx);
                if (c->ctx) {
                    redisReply* r = (redisReply*)redisCommand(c->ctx, "PING");
                    if (!r || r->type != REDIS_REPLY_STATUS) {
                        fprintf(stderr, "[Redis:health] master conn %zu dead, reconnecting\n", i);
                        redisFree(c->ctx); c->ctx = nullptr;
                        ReconnectOne(*c);
                    }
                    if (r) freeReplyObject(r);
                }
            }

            for (size_t i = 0; i < slave_pool_.size(); ++i) {
                auto& c = slave_pool_[i];
                std::lock_guard<std::mutex> lock(c->mtx);
                if (c->ctx) {
                    redisReply* r = (redisReply*)redisCommand(c->ctx, "PING");
                    if (!r || r->type != REDIS_REPLY_STATUS) {
                        fprintf(stderr, "[Redis:health] slave conn %zu dead, reconnecting\n", i);
                        redisFree(c->ctx); c->ctx = nullptr;
                        ConnectOne(*c, slave_host_, slave_port_);
                    }
                    if (r) freeReplyObject(r);
                }
            }
        }
    });
    printf("[Redis] Health check started (every 30s)\n");
}

void RedisClient::StopHealthCheck() {
    health_running_ = false;
    if (health_check_.joinable()) health_check_.join();
}
