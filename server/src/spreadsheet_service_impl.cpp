#include "spreadsheet_service_impl.h"
#include "auth_interceptor.h"
#include "database.h"
#include "redis_client.h"
#include "call_logger.h"
#include "system_logger.h"
#include <cstdio>
#include <chrono>
#include <thread>

using json = rpc_json::Value;

// Extract the username carried in gRPC metadata (set by the gateway for logging).
static std::string UsernameFromMeta(grpc::ServerContext* ctx) {
    auto it = ctx->client_metadata().find("username");
    if (it != ctx->client_metadata().end())
        return std::string(it->second.data(), it->second.length());
    return "";
}

// Redis key helpers — keyed by user_id so username changes never corrupt cache.
static std::string VersionKey(int64_t user_id) {
    return "user:" + std::to_string(user_id) + ":sheets:version";
}
static std::string ListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    std::string key = "user:" + std::to_string(user_id) + ":sheets:v" + std::to_string(version);
    if (page_size > 0) key += ":p" + std::to_string(page) + ":ps" + std::to_string(page_size);
    return key;
}

grpc::Status SpreadsheetServiceImpl::CreateSpreadsheet(
    grpc::ServerContext* context, const rpc::CreateSpreadsheetRequest* req,
    rpc::CreateSpreadsheetResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (req->name().empty()) {
        resp->set_success(false);
        resp->set_error("Name is required");
        return grpc::Status::OK;
    }

    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    int64_t id = 0;
    bool ok = db_->CreateSpreadsheet(req->user_id(), username, req->name(),
                                     req->description(), req->headers_json(),
                                     req->data_json(), id, req->idempotency_key());

    if (!ok) {
        resp->set_success(false);
        resp->set_error("Failed to create spreadsheet");
        if (logger_) {
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - start).count();
            json p, r;
            p["name"] = json(req->name());
            r["error"] = json("Failed to create spreadsheet");
            logger_->Log(username, "SpreadsheetService", "Create", p, r, false, dur);
        }
        return grpc::Status::OK;
    }

    // Invalidate list cache
    if (redis_ && redis_->IsConnected()) {
        redis_->Increment(VersionKey(req->user_id()));
        if (slog_) LOG_DEBUG(*slog_, "Create id=" + std::to_string(id) + " INCR version for uid=" + std::to_string(req->user_id()));
    }

    resp->set_success(true);
    resp->set_id(id);

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["name"] = json(req->name());
        r["id"] = json(static_cast<double>(id));
        logger_->Log(username, "SpreadsheetService", "Create", p, r, true, dur);
    }
    if (slog_) LOG_INFO(*slog_, "Created id=" + std::to_string(id) + " by " + username + " (uid=" + std::to_string(req->user_id()) + ")");
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::GetSpreadsheet(
    grpc::ServerContext* context, const rpc::GetSpreadsheetRequest* req,
    rpc::GetSpreadsheetResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    const std::string cache_key = "sheet:" + std::to_string(req->id());
    const std::string ts_key    = cache_key + ":ts";
    const std::string lock_key  = "lock:sheet:" + std::to_string(req->id());
    const std::string kNullMarker = "__NULL__";
    const int  LOGICAL_TTL  = 300;   // 逻辑过期 5min，超时触发异步刷新
    const int  PHYSICAL_TTL = 3600;  // 物理过期 1h，防止内存无限增长
    const int  NULL_TTL     = 60;    // 空值缓存 1min
    const int  LOCK_TTL     = 10;    // 刷新锁 10s，防死锁

    // 异步刷新函数：查 MySQL → 回写 Redis(data+ts) → 释放锁
    int64_t req_uid = req->user_id();
    auto async_refresh = [this, kNullMarker, NULL_TTL, PHYSICAL_TTL](int64_t id, int64_t uid, std::string ck, std::string tk, std::string lk) {
        SpreadsheetRow row;
        if (db_ && db_->GetSpreadsheet(id, uid, row)) {
            rpc::GetSpreadsheetResponse fresh;
            auto* s = fresh.mutable_spreadsheet();
            s->set_id(row.id);
            s->set_username(row.username);
            s->set_name(row.name);
            s->set_description(row.description);
            s->set_headers_json(row.headers_json);
            s->set_data_json(row.data_json);
            s->set_row_count(row.row_count);
            s->set_col_count(row.col_count);
            s->set_created_at(row.created_at);
            s->set_updated_at(row.updated_at);
            fresh.set_success(true);
            std::string ser;
            if (fresh.SerializeToString(&ser) && redis_ && redis_->IsConnected()) {
                redis_->SetJSON(ck, ser, PHYSICAL_TTL);
                redis_->SetJSON(tk, std::to_string(std::time(nullptr)), PHYSICAL_TTL);
                if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(id) + " ASYNC-REFRESHED key=" + ck);
            }
        } else if (db_ && redis_ && redis_->IsConnected()) {
            // Not found — cache null marker to prevent penetration
            redis_->SetJSON(ck, kNullMarker, NULL_TTL);
            redis_->SetJSON(tk, std::to_string(std::time(nullptr)), NULL_TTL);
            if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(id) + " ASYNC-NULL-CACHED key=" + ck);
        }
        if (redis_ && redis_->IsConnected()) {
            redis_->DeleteKey(lk);
        }
    };

    // 1) Try Redis cache
    if (redis_ && redis_->IsConnected()) {
        std::string cached;
        if (redis_->GetJSON(cache_key, cached)) {
            // 1a) Null-cache hit — penetration protection
            if (cached == kNullMarker) {
                resp->set_success(false);
                resp->set_error("Not found");
                resp->set_cache_source("redis");
                if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " NULL-HIT key=" + cache_key);
                return grpc::Status::OK;
            }
            // 1b) Data cache hit
            if (resp->ParseFromString(cached)) {
                resp->set_cache_source("redis");

                // 1c) Check logical expiration
                std::string ts_str;
                int64_t now_ts = std::time(nullptr);
                bool need_refresh = false;

                if (!redis_->GetJSON(ts_key, ts_str)) {
                    need_refresh = true;
                } else {
                    int64_t cached_ts = 0;
                    try { cached_ts = std::stoll(ts_str); } catch (...) { need_refresh = true; }
                    if (!need_refresh && (now_ts - cached_ts > LOGICAL_TTL)) {
                        need_refresh = true;
                    }
                }

                // 1d) Trigger async refresh if logically expired (non-blocking)
                if (need_refresh && redis_->SetNX(lock_key, "1", LOCK_TTL)) {
                    if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " STALE (logical-expire), async refresh triggered");
                    std::thread(async_refresh, req->id(), req_uid, cache_key, ts_key, lock_key).detach();
                }

                if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id())
                    + " HIT key=" + cache_key + (need_refresh ? " (stale)" : " (fresh)"));
                if (logger_) {
                    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - start).count();
                    json p, r;
                    p["id"] = json(static_cast<double>(req->id()));
                    r["cache"] = json("redis");
                    logger_->Log(username, "SpreadsheetService", "Get", p, r, true, dur);
                }
                return grpc::Status::OK;  // 立即返回，不等待刷新
            }
            if (slog_) LOG_ERROR(*slog_, "Get id=" + std::to_string(req->id()) + " ParseFromString FAILED key=" + cache_key);
        } else {
            if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " MISS key=" + cache_key);
        }
    }

    // 2) Cold start: Redis MISS → MySQL
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    SpreadsheetRow row;
    if (!db_->GetSpreadsheet(req->id(), req_uid, row)) {
        // Cache null sentinel to prevent cache penetration
        if (redis_ && redis_->IsConnected()) {
            redis_->SetJSON(cache_key, kNullMarker, NULL_TTL);
            redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), NULL_TTL);
            if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " NULL-CACHED key=" + cache_key);
        }
        resp->set_success(false);
        resp->set_error("Not found");
        if (logger_) {
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - start).count();
            json p, r;
            p["id"] = json(static_cast<double>(req->id()));
            r["error"] = json("Not found");
            logger_->Log(username, "SpreadsheetService", "Get", p, r, false, dur);
        }
        return grpc::Status::OK;
    }

    // 3) Populate response from MySQL
    auto* sheet = resp->mutable_spreadsheet();
    sheet->set_id(row.id);
    sheet->set_username(row.username);
    sheet->set_name(row.name);
    sheet->set_description(row.description);
    sheet->set_headers_json(row.headers_json);
    sheet->set_data_json(row.data_json);
    sheet->set_row_count(row.row_count);
    sheet->set_col_count(row.col_count);
    sheet->set_created_at(row.created_at);
    sheet->set_updated_at(row.updated_at);
    resp->set_success(true);
    resp->set_cache_source("mysql");

    // 4) Populate Redis cache (with both data and timestamp)
    if (redis_ && redis_->IsConnected()) {
        std::string serialized;
        if (resp->SerializeToString(&serialized)) {
            redis_->SetJSON(cache_key, serialized, PHYSICAL_TTL);
            redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), PHYSICAL_TTL);
            if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " POPULATED key=" + cache_key + " ttl=" + std::to_string(PHYSICAL_TTL) + "s");
        }
    }

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        r["cache"] = json("mysql");
        logger_->Log(username, "SpreadsheetService", "Get", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::ListSpreadsheets(
    grpc::ServerContext* context, const rpc::ListSpreadsheetsRequest* req,
    rpc::ListSpreadsheetsResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);
    int64_t uid = req->user_id();
    int page      = req->page();
    int page_size = req->page_size();

    std::string version_key = VersionKey(uid);

    // 1) Try Redis cache
    if (redis_ && redis_->IsConnected()) {
        int64_t version = redis_->GetInt(version_key);
        std::string cache_key = ListCacheKey(uid, version, page, page_size);
        std::string cached;
        if (redis_->GetJSON(cache_key, cached)) {
            if (resp->ParseFromString(cached)) {
                if (slog_) LOG_DEBUG(*slog_, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) + " HIT key=" + cache_key);
                resp->set_cache_source("redis");
                if (logger_) {
                    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - start).count();
                    json p, r;
                    r["cache"] = json("redis");
                    logger_->Log(username, "SpreadsheetService", "List", p, r, true, dur);
                }
                return grpc::Status::OK;
            }
            if (slog_) LOG_ERROR(*slog_, "List uid=" + std::to_string(uid) + " ParseFromString FAILED key=" + cache_key);
        } else {
            if (slog_) LOG_DEBUG(*slog_, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) + " MISS key=" + cache_key);
        }
    }

    // 2) Fallback to MySQL
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    std::vector<SpreadsheetSummary> sheets;
    int total = 0;
    if (!db_->ListSpreadsheets(uid, sheets, total, page, page_size)) {
        resp->set_success(false);
        resp->set_error("Query failed");
        return grpc::Status::OK;
    }

    for (const auto& s : sheets) {
        auto* summary = resp->add_sheets();
        summary->set_id(s.id);
        summary->set_name(s.name);
        summary->set_description(s.description);
        summary->set_row_count(s.row_count);
        summary->set_col_count(s.col_count);
        summary->set_updated_at(s.updated_at);
    }
    resp->set_total(total);
    resp->set_success(true);
    resp->set_cache_source("mysql");

    // 3) Populate Redis cache
    if (redis_ && redis_->IsConnected()) {
        int64_t version = redis_->GetInt(version_key);
        std::string cache_key = ListCacheKey(uid, version, page, page_size);
        std::string serialized;
        if (resp->SerializeToString(&serialized)) {
            if (redis_->SetJSON(cache_key, serialized, 120)) {
                if (slog_) LOG_DEBUG(*slog_, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) + " POPULATED key=" + cache_key + " size=" + std::to_string(serialized.size()) + " ttl=120");
            }
        }
    }

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        r["total"] = json(static_cast<double>(total));
        r["cache"] = json("mysql");
        logger_->Log(username, "SpreadsheetService", "List", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::UpdateSpreadsheet(
    grpc::ServerContext* context, const rpc::UpdateSpreadsheetRequest* req,
    rpc::UpdateSpreadsheetResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    const int MAX_RETRIES = 3;
    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        int64_t owner_uid = 0;
        int version = 0;
        if (!db_->GetSpreadsheetOwner(req->id(), owner_uid, &version) || owner_uid != req->user_id()) {
            resp->set_success(false);
            resp->set_error("Not found or permission denied");
            return grpc::Status::OK;
        }

        bool ok = db_->UpdateSpreadsheet(req->id(), req->name(), req->description(),
                                         req->headers_json(), req->data_json(), version);

        if (ok) {
            if (redis_ && redis_->IsConnected()) {
                redis_->DeleteKey("sheet:" + std::to_string(req->id()));
                redis_->DeleteKey("sheet:" + std::to_string(req->id()) + ":ts");
                redis_->Increment(VersionKey(req->user_id()));
                if (slog_) LOG_DEBUG(*slog_, "Update id=" + std::to_string(req->id()) + " INVALIDATED + INCR version uid=" + std::to_string(req->user_id()));
            }

            resp->set_success(true);

            if (logger_) {
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now() - start).count();
                json p, r;
                p["id"] = json(static_cast<double>(req->id()));
                logger_->Log(username, "SpreadsheetService", "Update", p, r, true, dur);
            }
            if (slog_) LOG_INFO(*slog_, "Updated id=" + std::to_string(req->id()) + " by " + username);
            return grpc::Status::OK;
        }

        if (slog_) LOG_WARN(*slog_, "Update id=" + std::to_string(req->id()) + " version conflict, retry " + std::to_string(attempt + 1) + "/" + std::to_string(MAX_RETRIES));
    }

    resp->set_success(false);
    resp->set_error("Update conflict, please retry");
    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        r["error"] = json("Update conflict");
        logger_->Log(username, "SpreadsheetService", "Update", p, r, false, dur);
    }
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::DeleteSpreadsheet(
    grpc::ServerContext* context, const rpc::DeleteSpreadsheetRequest* req,
    rpc::DeleteSpreadsheetResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    int64_t owner_uid = 0;
    if (!db_->GetSpreadsheetOwner(req->id(), owner_uid) || owner_uid != req->user_id()) {
        resp->set_success(false);
        resp->set_error("Not found or permission denied");
        return grpc::Status::OK;
    }

    bool ok = db_->DeleteSpreadsheet(req->id());

    if (!ok) {
        resp->set_success(false);
        resp->set_error("Failed to delete");
        if (logger_) {
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - start).count();
            json p, r;
            p["id"] = json(static_cast<double>(req->id()));
            r["error"] = json("Failed to delete");
            logger_->Log(username, "SpreadsheetService", "Delete", p, r, false, dur);
        }
        return grpc::Status::OK;
    }

    if (redis_ && redis_->IsConnected()) {
        redis_->DeleteKey("sheet:" + std::to_string(req->id()));
        redis_->DeleteKey("sheet:" + std::to_string(req->id()) + ":ts");
        redis_->Increment(VersionKey(req->user_id()));
        if (slog_) LOG_DEBUG(*slog_, "Delete id=" + std::to_string(req->id()) + " INVALIDATED + INCR version uid=" + std::to_string(req->user_id()));
    }

    resp->set_success(true);

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        logger_->Log(username, "SpreadsheetService", "Delete", p, r, true, dur);
    }
    if (slog_) LOG_INFO(*slog_, "Deleted id=" + std::to_string(req->id()) + " by " + username);
    return grpc::Status::OK;
}
