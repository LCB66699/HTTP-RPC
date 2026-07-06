#pragma once
#include <cstdio>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "shared/helper/handler_helpers.h"
#include "shared/helper/cache_helpers.h"
#include "shared/base/call_logger.h"
#include "shared/base/system_logger.h"

// ======== CreateSpec ========

struct CreateSpec {
    const char *event_type;        // e.g. "file.uploaded"
    const char *event_id_field;    // e.g. "file_id" or "id"
    const char *service_name;      // e.g. "FileService"
    std::string version_key;       // version key for list cache
};

// ======== FinishCreate ========

// Tail-end of Create handlers: event -> cache invalidation -> Redis -> response -> log.
// Caller handles auth guard, MinIO upload, DB create and rollback.
template <typename Resp>
void FinishCreate(Resp *resp, int64_t id, int64_t user_id,
                  const std::string &username, IDatabase *db,
                  IRabbitPublisher *rabbit, IRedisClient *redis,
                  CallLogger *logger, SystemLogger *slog,
                  const CreateSpec &spec, nlohmann::json extra_event,
                  int64_t elapsed_us, L1Cache *l1 = nullptr) {
    // Event: RabbitMQ + outbox
    {
        nlohmann::json ev;
        ev["type"] = spec.event_type;
        ev[spec.event_id_field] = id;
        for (auto it = extra_event.begin(); it != extra_event.end(); ++it)
            ev[it.key()] = it.value();
        PublishEvent(rabbit, db, user_id, spec.event_type, std::move(ev));
    }

    // Cache invalidation: sync L1 + Pub/Sub + outbox fallback
    InvalidateCaches(db, user_id, {spec.version_key}, l1, redis);

    // Redis version bump
    if (redis && redis->IsConnected())
        redis->Increment(spec.version_key);

    // Response
    resp->set_success(true);
    resp->set_id(id);

    // Logging
    if (logger) {
        json p{{"name", extra_event.value("original_name", extra_event.value("name", ""))}};
        json r{{"id", json(static_cast<double>(id))}};
        logger->Log(username, spec.service_name, "Create", p, r, true, elapsed_us);
    }
    if (slog)
        LOG_INFO(*slog, "Created id=" + std::to_string(id) + " by " + username +
                            " (uid=" + std::to_string(user_id) + ")");
}

// ======== List helpers ========

// TryListCache — attempt to serve list from Redis cache. Returns true on hit.
template <typename Resp>
bool TryListCache(Resp *resp, int64_t uid, int limit, const std::string &cursor,
                  IRedisClient *redis, const char *resource, SystemLogger *slog) {
    if (!redis || !redis->IsConnected()) return false;
    int64_t version = redis->GetInt(ResourceVersionKey(resource, uid));
    std::string cache_key = ResourceListCacheKey(resource, uid, version, limit, cursor);
    std::string cached;
    if (redis->GetJSON(cache_key, cached) && resp->ParseFromString(cached)) {
        resp->set_cache_source("redis");
        if (slog)
            LOG_DEBUG(*slog, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) +
                              " HIT key=" + cache_key);
        return true;
    }
    return false;
}

// PopulateListCache — write MySQL result back to Redis for future requests.
template <typename Resp>
void PopulateListCache(Resp *resp, int64_t uid, int limit, const std::string &cursor,
                       IRedisClient *redis, const char *resource, SystemLogger *slog) {
    if (!redis || !redis->IsConnected()) return;
    int64_t version = redis->GetInt(ResourceVersionKey(resource, uid));
    std::string cache_key = ResourceListCacheKey(resource, uid, version, limit, cursor);
    std::string serialized;
    if (resp->SerializeToString(&serialized)) {
        redis->SetJSON(cache_key, serialized, 120);
        if (slog)
            LOG_DEBUG(*slog, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) +
                              " POPULATED key=" + cache_key + " size=" + std::to_string(serialized.size()) + " ttl=120");
    }
}

// ======== Owner check ========

// CheckOwnerWithRetry — verify resource ownership with 3x retry + backoff.
// Usage:
//   int64_t owner_uid = 0;
//   if (!CheckOwnerWithRetry(id, caller_uid, db_,
//         [&](auto id, auto& uid) { return db_->GetFileOwner(id, uid); }, slog_))
//     return WriteResult(resp, HandlerResult<>::Fail("Not found", rpc_error::NOT_FOUND)), grpc::Status::OK;
template <typename Fn>
bool CheckOwnerWithRetry(int64_t id, int64_t caller_uid, IDatabase *db,
                         Fn &&get_owner, SystemLogger *slog) {
    if (caller_uid <= 0) return false;
    int64_t owner_uid = 0;
    bool found = false;
    for (int retry = 0; retry < 3 && db; ++retry) {
        found = get_owner(id, owner_uid);
        if (found && owner_uid != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50 << retry));
    }
    if (slog)
        LOG_DEBUG(*slog, "CheckOwner id=" + std::to_string(id) +
                             " owner=" + std::to_string(owner_uid) + " req_uid=" + std::to_string(caller_uid));
    return found && owner_uid == caller_uid;
}

// ======== CAS Update ========

struct CasConfig {
    int max_retries      = 7;
    int base_backoff_ms  = 50;   // initial delay
    int max_backoff_ms   = 1000; // cap at 1s per retry
};

// UpdateWithCAS — read latest version, attempt write, backoff-retry on conflict.
// The backend handles all retries internally; the caller (frontend) never makes
// retry decisions.  Returns true on success.  On final failure, invokes
// on_conflict(latest_version) so the response can carry the current version.
//
// read_ver(id, owner, version):  fetch owner + current version
// do_write(id, version):         attempt UPDATE WHERE version=?
// on_conflict(latest_ver):       called once when all retries exhausted
template <typename ReadVer, typename DoWrite, typename OnConflict>
bool UpdateWithCAS(int64_t id, int64_t caller_uid,
                   ReadVer read_ver, DoWrite do_write,
                   OnConflict on_conflict,
                   const CasConfig &cfg = {},
                   bool skip_owner_check = false) {
    for (int attempt = 0; attempt < cfg.max_retries; ++attempt) {
        int64_t owner = 0;
        int version = 0;
        if (!read_ver(id, owner, version))
            return false;
        if (!skip_owner_check && owner != caller_uid)
            return false;

        if (do_write(id, version))
            return true;

        if (attempt < cfg.max_retries - 1) {
            int delay = cfg.base_backoff_ms * (1 << attempt);
            if (delay > cfg.max_backoff_ms)
                delay = cfg.max_backoff_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        } else {
            int64_t _owner = 0;
            int latest = 0;
            read_ver(id, _owner, latest);
            on_conflict(latest);
        }
    }
    return false;
}

// ======== DeleteSpec ========

struct DeleteSpec {
    const char *event_type;        // e.g. "file.deleted"
    const char *event_id_field;    // e.g. "file_id" or "id"
    const char *service_name;      // e.g. "FileService"
    std::string cache_key;         // resource cache key
    std::string version_key;       // version key for list cache
};

// ======== HandleDelete ========

// Unified delete handler: ownership -> DB delete -> event -> cache invalidation.
// Auth guard must be called before (requireAuth / requireAuthWith).
template <typename Resp, typename Fn_Owner, typename Fn_Delete>
grpc::Status HandleDelete(Resp *resp, int64_t id, int64_t user_id,
                          const std::string &username, IDatabase *db,
                          IRabbitPublisher *rabbit, IRedisClient *redis,
                          CallLogger *logger, SystemLogger *slog,
                          Fn_Owner &&get_owner, Fn_Delete &&do_delete,
                          const DeleteSpec &spec, L1Cache *l1 = nullptr) {
    ScopeTimer timer;

    if (!db) {
        WriteResult(resp, HandlerResult<>::Fail("Database not available", rpc_error::UNAVAILABLE));
        return grpc::Status::OK;
    }

    int64_t owner_uid = 0;
    bool found = get_owner(id, owner_uid);
    fprintf(stderr, "[Delete] id=%ld req_uid=%ld owner_uid=%ld found=%d\n",
            (long)id, (long)user_id, (long)owner_uid, found);
    if (!found || owner_uid != user_id) {
        WriteResult(resp, HandlerResult<>::Fail("Not found or permission denied", rpc_error::NOT_FOUND));
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Not found or permission denied");
    }

    if (!do_delete(id, user_id)) {
        WriteResult(resp, HandlerResult<>::Fail("Failed to delete", rpc_error::INTERNAL));
        return grpc::Status::OK;
    }

    // Event: RabbitMQ + outbox
    {
        nlohmann::json ev;
        ev["type"] = spec.event_type;
        ev[spec.event_id_field] = id;
        PublishEvent(rabbit, db, user_id, spec.event_type, std::move(ev));
    }

    // Cache invalidation: sync L1 + Pub/Sub + outbox fallback
    InvalidateCaches(db, user_id, {spec.cache_key, spec.version_key}, l1, redis);

    // Redis direct invalidation (L2)
    if (redis && redis->IsConnected()) {
        redis->DeleteKey(spec.cache_key);
        redis->DeleteKey(spec.cache_key + ":ts");
        redis->Increment(spec.version_key);
    }

    resp->set_success(true);

    if (logger) {
        json p{{"id", json(static_cast<double>(id))}};
        logger->Log(username, spec.service_name, "Delete", p, json{}, true, timer.elapsedUs());
    }
    if (slog)
        LOG_INFO(*slog, "Deleted id=" + std::to_string(id) + " by " + username);
    return grpc::Status::OK;
}
