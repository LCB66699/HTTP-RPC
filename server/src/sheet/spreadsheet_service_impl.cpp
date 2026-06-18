#include "spreadsheet_service_impl.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "auth_interceptor.h"
#include "call_logger.h"
#include "database.h"
#include "error_codes.h"
#include "l1_cache.h"
#include "redis_client.h"
#include "sheet_helpers.h"
#include "system_logger.h"

// Extract the username carried in gRPC metadata (set by the gateway for
// logging).
std::string UsernameFromMeta(grpc::ServerContext *ctx) {
    auto it = ctx->client_metadata().find("username");
    if (it != ctx->client_metadata().end())
        return std::string(it->second.data(), it->second.length());
    return "";
}

// 调用 Auth 服务验证调用者身份
bool SpreadsheetServiceImpl::ValidateCaller(grpc::ServerContext *ctx, int64_t user_id, std::string &out_username,
                                            std::string &out_role) const {
    if (!auth_stub_)
        return true;  // 未配置 Auth 通道时跳过验证（向后兼容）

    std::string token;
    auto it = ctx->client_metadata().find("authorization");
    if (it != ctx->client_metadata().end()) {
        std::string val(it->second.data(), it->second.length());
        const std::string prefix = "Bearer ";
        if (val.rfind(prefix, 0) == 0)
            token = val.substr(prefix.size());
        else
            token = val;
    }
    if (token.empty())
        return false;

    rpc::ValidateUserRequest vu_req;
    rpc::ValidateUserResponse vu_resp;
    vu_req.set_token(token);
    vu_req.set_user_id(user_id);

    grpc::ClientContext vu_ctx;
    vu_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto st = auth_stub_->ValidateUser(&vu_ctx, vu_req, &vu_resp);

    if (!st.ok() || !vu_resp.valid())
        return false;
    out_username = vu_resp.username();
    out_role = vu_resp.role();
    return true;
}


grpc::Status SpreadsheetServiceImpl::CreateSpreadsheet(grpc::ServerContext *context,
                                                       const rpc::CreateSpreadsheetRequest *req,
                                                       rpc::CreateSpreadsheetResponse *resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated)
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }

    // 服务间调用: 向 Auth 服务二次验证用户身份
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");

    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (req->name().empty()) {
        resp->set_success(false);
        SET_ERROR(resp, "Name is required", rpc_error::BAD_REQUEST);
        return grpc::Status::OK;
    }

    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    // MinIO: 上传表格 JSON → 元数据存 MySQL storage_path；回退 MySQL JSON 列
    std::string storage_key;
    if (minio_ && minio_->IsConfigured()) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::high_resolution_clock::now().time_since_epoch())
                      .count();
        storage_key = std::to_string(req->user_id()) + "/" + std::to_string(us) + ".json";
        nlohmann::json content;
        content["headers"] = nlohmann::json::parse(req->headers_json().empty() ? "[]" : req->headers_json());
        content["data"] = nlohmann::json::parse(req->data_json().empty() ? "[]" : req->data_json());
        std::string body = content.dump();
        if (!minio_->PutObject(storage_key, body, "application/json")) {
            resp->set_success(false);
            SET_ERROR(resp, "MinIO upload failed", rpc_error::INTERNAL);
            return grpc::Status::OK;
        }
    }

    int64_t id = 0;
    if (slog_) LOG_DEBUG(*slog_, "Create req->user_id=" + std::to_string(req->user_id()));
    bool ok = db_->CreateSpreadsheet(req->user_id(), username, req->name(), req->description(), req->headers_json(),
                                     req->data_json(), id, req->idempotency_key());
    if (!ok && !storage_key.empty()) {
        minio_->DeleteObject(storage_key);  // rollback
    }

    if (!ok) {
        resp->set_success(false);
        SET_ERROR(resp, "Failed to create spreadsheet", rpc_error::INTERNAL);
        if (logger_) {
            auto dur =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                    .count();
            json p, r;
            p["name"] = json(req->name());
            r["error"] = json("Failed to create spreadsheet");
            logger_->Log(username, "SpreadsheetService", "Create", p, r, false, dur);
        }
        return grpc::Status::OK;
    }

    // Write storage_path back to MySQL after MinIO upload
    if (!storage_key.empty() && db_) {
        db_->UpdateSpreadsheetStoragePath(id, storage_key);
    }

    // Invalidate list cache
    if (redis_ && redis_->IsConnected()) {
        redis_->Increment(SheetVersionKey(req->user_id()));
        if (slog_)
            LOG_DEBUG(*slog_,
                      "Create id=" + std::to_string(id) + " INCR version for uid=" + std::to_string(req->user_id()));
    }

    {
        nlohmann::json ev;
        ev["type"] = "sheet.created";
        ev["id"] = id;
        ev["user_id"] = req->user_id();
        ev["name"] = req->name();
        if (!storage_key.empty())
            ev["object_key"] = storage_key;
        if (rabbit_)
            rabbit_->Publish("rpc.events", "sheet.created", ev.dump());
        if (db_)
            db_->InsertOutbox(req->user_id(), "sheet.created", ev.dump());
    }

    resp->set_success(true);
    resp->set_id(id);

    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p, r;
        p["name"] = json(req->name());
        r["id"] = json(static_cast<double>(id));
        logger_->Log(username, "SpreadsheetService", "Create", p, r, true, dur);
    }
    if (slog_)
        LOG_INFO(*slog_, "Created id=" + std::to_string(id) + " by " + username +
                             " (uid=" + std::to_string(req->user_id()) + ")");
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::GetSpreadsheet(grpc::ServerContext *context, const rpc::GetSpreadsheetRequest *req,
                                                    rpc::GetSpreadsheetResponse *resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated)
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    // 校验资源归属：user_id 必须匹配 sheet 所有者
    if (req->user_id() <= 0) {
        resp->set_success(false);
        SET_ERROR(resp, "Not found", rpc_error::NOT_FOUND);
        return grpc::Status::OK;
    }
    int64_t owner_uid = 0;
    bool found = false;
    // 连接池不同连接可能有瞬时可见性差异，重试 3 次
    for (int retry = 0; retry < 3 && db_; ++retry) {
        found = db_->GetSpreadsheetOwner(req->id(), owner_uid);
        if (found && owner_uid != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (slog_) LOG_DEBUG(*slog_, "GetOwner id=" + std::to_string(req->id()) +
                                     " owner=" + std::to_string(owner_uid) + " req_uid=" + std::to_string(req->user_id()));
    if (found && owner_uid != req->user_id()) {
        if (slog_) LOG_DEBUG(*slog_, "Owner mismatch! sheet_id=" + std::to_string(req->id()) +
                                      " owner=" + std::to_string(owner_uid) + " caller=" + std::to_string(req->user_id()));
    }
    if (!found || owner_uid != req->user_id()) {
        resp->set_success(false);
        SET_ERROR(resp, "Not found", rpc_error::NOT_FOUND);
        return grpc::Status::OK;
    }
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    int64_t req_uid = req->user_id();
    const std::string cache_key = "u:" + std::to_string(req_uid) + ":sheet:" + std::to_string(req->id());
    const std::string ts_key = cache_key + ":ts";
    const std::string lock_key = "lock:u:" + std::to_string(req_uid) + ":sheet:" + std::to_string(req->id());
    const std::string kNullMarker = "__NULL__";
    const int LOGICAL_TTL = 300;    // 逻辑过期 5min，超时触发异步刷新
    const int PHYSICAL_TTL = 3600;  // 物理过期 1h，防止内存无限增长
    const int NULL_TTL = 60;        // 空值缓存 1min
    const int LOCK_TTL = 10;        // 刷新锁 10s，防死锁

    // 辅助函数：从 MinIO 回填 headers/data
    auto fillFromMinIO = [this](SpreadsheetRow &row) {
        if (minio_ && minio_->IsConfigured() && !row.storage_path.empty()) {
            std::string body;
            if (minio_->GetObject(row.storage_path, body)) {
                try {
                    auto j = nlohmann::json::parse(body);
                    row.headers_json = j.value("headers", nlohmann::json::array()).dump();
                    row.data_json = j.value("data", nlohmann::json::array()).dump();
                } catch (...) {
                }
            }
        }
    };

    // 异步刷新函数：查 MySQL → 回写 Redis(data+ts) → 释放锁
    auto async_refresh = [this, kNullMarker, &fillFromMinIO](int64_t id, int64_t uid, std::string ck, std::string tk,
                                                             std::string lk) {
        SpreadsheetRow row;
        if (db_ && db_->GetSpreadsheet(id, uid, row)) {
            fillFromMinIO(row);
            rpc::GetSpreadsheetResponse fresh;
            auto *s = fresh.mutable_spreadsheet();
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
                redis_->SetJSON(ck, ser, RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                redis_->SetJSON(tk, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                if (slog_)
                    LOG_DEBUG(*slog_, "Get id=" + std::to_string(id) + " ASYNC-REFRESHED key=" + ck);
            }
        } else if (db_ && redis_ && redis_->IsConnected()) {
            // Not found — cache null marker to prevent penetration
            redis_->SetJSON(ck, kNullMarker, RedisClient::JitteredTTL(NULL_TTL, 30));
            redis_->SetJSON(tk, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(NULL_TTL, 30));
            if (slog_)
                LOG_DEBUG(*slog_, "Get id=" + std::to_string(id) + " ASYNC-NULL-CACHED key=" + ck);
        }
        if (redis_ && redis_->IsConnected()) {
            redis_->DeleteKey(lk);
        }
    };

    // 1) Try L1 local cache
    if (l1_) {
        auto l1_val = l1_->Get(cache_key);
        if (l1_val) {
            if (*l1_val == kNullMarker) {
                resp->set_success(false);
                SET_ERROR(resp, "Not found", rpc_error::NOT_FOUND);
                resp->set_cache_source("l1");
                return grpc::Status::OK;
            }
            if (resp->ParseFromString(*l1_val)) {
                resp->set_cache_source("l1");
                if (slog_)
                    LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " L1-HIT");
                return grpc::Status::OK;
            }
        }
    }

    // 2) Try Redis cache
    if (redis_ && redis_->IsConnected()) {
        std::string cached;
        if (redis_->GetJSON(cache_key, cached)) {
            // 1a) Null-cache hit — penetration protection
            if (cached == kNullMarker) {
                resp->set_success(false);
                SET_ERROR(resp, "Not found", rpc_error::NOT_FOUND);
                resp->set_cache_source("redis");
                if (slog_)
                    LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " NULL-HIT key=" + cache_key);
                return grpc::Status::OK;
            }
            // 1b) Data cache hit
            if (resp->ParseFromString(cached)) {
                resp->set_cache_source("redis");
                if (l1_)
                    l1_->Set(cache_key, cached);  // L1 backfill

                // 1c) Check logical expiration
                std::string ts_str;
                int64_t now_ts = std::time(nullptr);
                bool need_refresh = false;

                if (!redis_->GetJSON(ts_key, ts_str)) {
                    need_refresh = true;
                } else {
                    int64_t cached_ts = 0;
                    try {
                        cached_ts = std::stoll(ts_str);
                    } catch (...) {
                        need_refresh = true;
                    }
                    if (!need_refresh && (now_ts - cached_ts > LOGICAL_TTL)) {
                        need_refresh = true;
                    }
                }

                // 1d) Trigger async refresh if logically expired (non-blocking)
                if (need_refresh && redis_->SetNX(lock_key, "1", LOCK_TTL)) {
                    if (slog_)
                        LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) +
                                              " STALE (logical-expire), async refresh triggered");
                    std::thread([this, async_refresh, id = req->id(), uid = req_uid, ck = cache_key, tk = ts_key,
                                 lk = lock_key]() {
                        std::atomic<bool> done{false};
                        std::thread wd([this, &done, &lk]() {
                            while (!done) {
                                std::this_thread::sleep_for(std::chrono::seconds(5));
                                if (!done && redis_)
                                    redis_->ExpireKey(lk, 10);
                            }
                        });
                        async_refresh(id, uid, ck, tk, lk);
                        done = true;
                        wd.join();
                    }).detach();
                }

                if (slog_)
                    LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " HIT key=" + cache_key +
                                          (need_refresh ? " (stale)" : " (fresh)"));
                if (logger_) {
                    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::high_resolution_clock::now() - start)
                                   .count();
                    json p, r;
                    p["id"] = json(static_cast<double>(req->id()));
                    r["cache"] = json("redis");
                    logger_->Log(username, "SpreadsheetService", "Get", p, r, true, dur);
                }
                return grpc::Status::OK;  // 立即返回，不等待刷新
            }
            if (slog_)
                LOG_ERROR(*slog_, "Get id=" + std::to_string(req->id()) + " ParseFromString FAILED key=" + cache_key);
        } else {
            if (slog_)
                LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " MISS key=" + cache_key);
        }
    }

    // 2) Cold start: Redis MISS → MySQL
    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    SpreadsheetRow row;
    // 同连接可能瞬时不可见已提交数据，短暂重试
    bool got = false;
    for (int r = 0; r < 3 && db_; ++r) {
        got = db_->GetSpreadsheet(req->id(), req_uid, row);
        if (got) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!got) {
        // Cache null sentinel to prevent cache penetration
        if (redis_ && redis_->IsConnected()) {
            redis_->SetJSON(cache_key, kNullMarker, RedisClient::JitteredTTL(NULL_TTL, 30));
            redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(NULL_TTL, 30));
            if (slog_)
                LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " NULL-CACHED key=" + cache_key);
        }
        resp->set_success(false);
        SET_ERROR(resp, "Not found", rpc_error::NOT_FOUND);
        if (logger_) {
            auto dur =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                    .count();
            json p, r;
            p["id"] = json(static_cast<double>(req->id()));
            r["error"] = json("Not found");
            logger_->Log(username, "SpreadsheetService", "Get", p, r, false, dur);
        }
        return grpc::Status::OK;
    }

    // 3) Populate response (MinIO first, MySQL fallback)
    fillFromMinIO(row);
    auto *sheet = resp->mutable_spreadsheet();
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
            redis_->SetJSON(cache_key, serialized, RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
            redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
            if (l1_)
                l1_->Set(cache_key, serialized);  // L1 backfill
            if (slog_)
                LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " POPULATED key=" + cache_key +
                                      " ttl=" + std::to_string(PHYSICAL_TTL) + "s");
        }
    }

    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        r["cache"] = json("mysql");
        logger_->Log(username, "SpreadsheetService", "Get", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::ListSpreadsheets(grpc::ServerContext *context,
                                                      const rpc::ListSpreadsheetsRequest *req,
                                                      rpc::ListSpreadsheetsResponse *resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated)
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);
    int64_t uid = req->user_id();
    int page = req->page();
    int page_size = req->page_size();

    std::string version_key = SheetVersionKey(uid);

    // 1) Try Redis cache
    if (redis_ && redis_->IsConnected()) {
        int64_t version = redis_->GetInt(version_key);
        std::string cache_key = SheetListCacheKey(uid, version, page, page_size);
        std::string cached;
        if (redis_->GetJSON(cache_key, cached)) {
            if (resp->ParseFromString(cached)) {
                if (slog_)
                    LOG_DEBUG(*slog_, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) +
                                          " HIT key=" + cache_key);
                resp->set_cache_source("redis");
                if (logger_) {
                    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::high_resolution_clock::now() - start)
                                   .count();
                    json p, r;
                    r["cache"] = json("redis");
                    logger_->Log(username, "SpreadsheetService", "List", p, r, true, dur);
                }
                return grpc::Status::OK;
            }
            if (slog_)
                LOG_ERROR(*slog_, "List uid=" + std::to_string(uid) + " ParseFromString FAILED key=" + cache_key);
        } else {
            if (slog_)
                LOG_DEBUG(*slog_, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) +
                                      " MISS key=" + cache_key);
        }
    }

    // 2) Fallback to MySQL
    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    int64_t after_id = req->after_id();
    int limit = req->limit() > 0 ? req->limit() : (page_size > 0 ? page_size : 20);

    std::vector<SpreadsheetSummary> sheets;
    int total = 0;
    if (!db_->ListSpreadsheets(uid, sheets, total, page, limit, after_id)) {
        resp->set_success(false);
        SET_ERROR(resp, "Query failed", rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    for (const auto &s : sheets) {
        auto *summary = resp->add_sheets();
        summary->set_id(s.id);
        summary->set_name(s.name);
        summary->set_description(s.description);
        summary->set_row_count(s.row_count);
        summary->set_col_count(s.col_count);
        summary->set_updated_at(s.updated_at);
    }
    // Cursor pagination response
    if (after_id >= 0 && limit > 0) {
        resp->set_has_more((int)sheets.size() == limit);
        if (!sheets.empty())
            resp->set_next_cursor(std::to_string(sheets.back().id));
    }
    resp->set_total(total);
    resp->set_success(true);
    resp->set_cache_source("mysql");

    // 3) Populate Redis cache
    if (redis_ && redis_->IsConnected()) {
        int64_t version = redis_->GetInt(version_key);
        std::string cache_key = SheetListCacheKey(uid, version, page, page_size);
        std::string serialized;
        if (resp->SerializeToString(&serialized)) {
            if (redis_->SetJSON(cache_key, serialized, 120)) {
                if (slog_)
                    LOG_DEBUG(*slog_, "List uid=" + std::to_string(uid) + " v" + std::to_string(version) +
                                          " POPULATED key=" + cache_key + " size=" + std::to_string(serialized.size()) +
                                          " ttl=120");
            }
        }
    }

    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p, r;
        r["total"] = json(static_cast<double>(total));
        r["cache"] = json("mysql");
        logger_->Log(username, "SpreadsheetService", "List", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::UpdateSpreadsheet(grpc::ServerContext *context,
                                                       const rpc::UpdateSpreadsheetRequest *req,
                                                       rpc::UpdateSpreadsheetResponse *resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated)
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    const int MAX_RETRIES = 3;
    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        int64_t owner_uid = 0;
        int version = 0;
        if (!db_->GetSpreadsheetOwner(req->id(), owner_uid, &version) || owner_uid != req->user_id()) {
            resp->set_success(false);
            SET_ERROR(resp, "Not found or permission denied", rpc_error::NOT_FOUND);
            return grpc::Status::OK;
        }

        bool ok = db_->UpdateSpreadsheet(req->id(), req->user_id(), req->name(), req->description(),
                                         req->headers_json(), req->data_json(), version);

        if (ok) {
            if (redis_ && redis_->IsConnected()) {
                redis_->DeleteKey("u:" + std::to_string(req->user_id()) + ":sheet:" + std::to_string(req->id()));
                redis_->DeleteKey("u:" + std::to_string(req->user_id()) + ":sheet:" + std::to_string(req->id()) +
                                  ":ts");
                redis_->Increment(SheetVersionKey(req->user_id()));
                if (slog_)
                    LOG_DEBUG(*slog_, "Update id=" + std::to_string(req->id()) +
                                          " INVALIDATED + INCR version uid=" + std::to_string(req->user_id()));
            }

            {
                nlohmann::json ev;
                ev["type"] = "sheet.updated";
                ev["id"] = req->id();
                ev["user_id"] = req->user_id();
                ev["name"] = req->name();
                ev["description"] = req->description();
                if (rabbit_)
                    rabbit_->Publish("rpc.events", "sheet.updated", ev.dump());
                if (db_)
                    db_->InsertOutbox(req->user_id(), "sheet.updated", ev.dump());
            }

            resp->set_success(true);

            if (logger_) {
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::high_resolution_clock::now() - start)
                               .count();
                json p, r;
                p["id"] = json(static_cast<double>(req->id()));
                logger_->Log(username, "SpreadsheetService", "Update", p, r, true, dur);
            }
            if (slog_)
                LOG_INFO(*slog_, "Updated id=" + std::to_string(req->id()) + " by " + username);
            return grpc::Status::OK;
        }

        if (slog_)
            LOG_WARN(*slog_, "Update id=" + std::to_string(req->id()) + " version conflict, retry " +
                                 std::to_string(attempt + 1) + "/" + std::to_string(MAX_RETRIES));
    }

    resp->set_success(false);
    SET_ERROR(resp, "Update conflict, please retry", rpc_error::CONFLICT);
    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        r["error"] = json("Update conflict");
        logger_->Log(username, "SpreadsheetService", "Update", p, r, false, dur);
    }
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::DeleteSpreadsheet(grpc::ServerContext *context,
                                                       const rpc::DeleteSpreadsheetRequest *req,
                                                       rpc::DeleteSpreadsheetResponse *resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated)
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    int64_t owner_uid = 0;
    if (!db_->GetSpreadsheetOwner(req->id(), owner_uid) || owner_uid != req->user_id()) {
        resp->set_success(false);
        SET_ERROR(resp, "Not found or permission denied", rpc_error::NOT_FOUND);
        return grpc::Status::OK;
    }

    bool ok = db_->DeleteSpreadsheet(req->id(), req->user_id());

    if (!ok) {
        resp->set_success(false);
        SET_ERROR(resp, "Failed to delete", rpc_error::INTERNAL);
        if (logger_) {
            auto dur =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                    .count();
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
        redis_->Increment(SheetVersionKey(req->user_id()));
        if (slog_)
            LOG_DEBUG(*slog_, "Delete id=" + std::to_string(req->id()) +
                                  " INVALIDATED + INCR version uid=" + std::to_string(req->user_id()));
    }

    {
        nlohmann::json ev;
        ev["type"] = "sheet.deleted";
        ev["id"] = req->id();
        ev["user_id"] = req->user_id();
        if (rabbit_)
            rabbit_->Publish("rpc.events", "sheet.deleted", ev.dump());
        if (db_)
            db_->InsertOutbox(req->user_id(), "sheet.deleted", ev.dump());
    }

    resp->set_success(true);

    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        logger_->Log(username, "SpreadsheetService", "Delete", p, r, true, dur);
    }
    if (slog_)
        LOG_INFO(*slog_, "Deleted id=" + std::to_string(req->id()) + " by " + username);
    return grpc::Status::OK;
}
