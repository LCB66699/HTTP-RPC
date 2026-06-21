#include "sheet/spreadsheet_service_impl.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "shared/base/rpc_interceptor.h"
#include "shared/base/call_logger.h"
#include "shared/cache/circuit_breaker.h"
#include "shared/client/database.h"
#include "shared/base/error_codes.h"
#include "shared/cache/l1_cache.h"
#include "shared/client/redis_client.h"
#include "shared/helper/cache_helpers.h"
#include "shared/helper/crud_helpers.h"
#include "shared/helper/handler_helpers.h"
#include "shared/base/system_logger.h"

// 璋冪敤 Auth 鏈嶅姟楠岃瘉璋冪敤鑰呰韩锟?
bool SpreadsheetServiceImpl::ValidateCaller(grpc::ServerContext *ctx, int64_t user_id, std::string &out_username,
                                            std::string &out_role) const {
    if (!auth_stub_)
        return true;  // 鏈厤锟?Auth 閫氶亾鏃惰烦杩囬獙璇侊紙鍚戝悗鍏煎锟?

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

    // 閫氳繃鐔旀柇鍣ㄨ皟锟?Auth 鏈嶅姟
    bool ok = auth_cb_.Call("auth.validate", [&]() -> bool {
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
    });
    return ok;
}


grpc::Status SpreadsheetServiceImpl::CreateSpreadsheet(grpc::ServerContext *context,
                                                       const rpc::CreateSpreadsheetRequest *req,
                                                       rpc::CreateSpreadsheetResponse *resp) {
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;
    ScopeTimer timer;
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

    // MinIO: 涓婁紶琛ㄦ牸 JSON 锟?鍏冩暟鎹瓨 MySQL storage_path锛涘洖閫€ MySQL JSON 锟?
    std::string storage_key;
    if (minio_ && minio_->IsConfigured()) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::high_resolution_clock::now().time_since_epoch())
                      .count();
        storage_key = std::to_string(g_rpc_auth_ctx.user_id) + "/" + std::to_string(us) + ".json";
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
    if (slog_) LOG_DEBUG(*slog_, "Create req->user_id=" + std::to_string(g_rpc_auth_ctx.user_id));
    bool ok = db_->CreateSpreadsheet(g_rpc_auth_ctx.user_id, username, req->name(), req->description(), req->headers_json(),
                                     req->data_json(), id, req->idempotency_key());
    if (!ok && !storage_key.empty()) {
        minio_->DeleteObject(storage_key);  // rollback
    }

    if (!ok) {
        resp->set_success(false);
        SET_ERROR(resp, "Failed to create spreadsheet", rpc_error::INTERNAL);
        if (logger_) {
            auto dur = timer.elapsedUs();
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

    nlohmann::json extra;
    extra["name"] = req->name();
    if (!storage_key.empty())
        extra["object_key"] = storage_key;
    FinishCreate(resp, id, g_rpc_auth_ctx.user_id, username, db_, rabbit_, redis_, logger_, slog_,
        {"sheet.created", "id", "SpreadsheetService", SheetVersionKey(g_rpc_auth_ctx.user_id)},
        std::move(extra), timer.elapsedUs());
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::GetSpreadsheet(grpc::ServerContext *context, const rpc::GetSpreadsheetRequest *req,
                                                    rpc::GetSpreadsheetResponse *resp) {
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;
    if (!CheckOwnerWithRetry(req->id(), g_rpc_auth_ctx.user_id, db_,
            [&](auto id, auto &uid) { return db_->GetSpreadsheetOwner(id, uid); }, slog_))
        return WriteResult(resp, HandlerResult<>::Fail("Not found", rpc_error::NOT_FOUND)), grpc::Status::OK;
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    int64_t req_uid = g_rpc_auth_ctx.user_id;
    const std::string cache_key = "u:" + std::to_string(req_uid) + ":sheet:" + std::to_string(req->id());
    const std::string ts_key = cache_key + ":ts";
    const std::string lock_key = "lock:u:" + std::to_string(req_uid) + ":sheet:" + std::to_string(req->id());
    const std::string kNullMarker = "__NULL__";
    const int LOGICAL_TTL = 300;    // 閫昏緫杩囨湡 5min锛岃秴鏃惰Е鍙戝紓姝ュ埛锟?
    const int PHYSICAL_TTL = 3600;  // 鐗╃悊杩囨湡 1h锛岄槻姝㈠唴瀛樻棤闄愬锟?
    const int NULL_TTL = 60;        // 绌哄€肩紦锟?1min
    const int LOCK_TTL = 10;        // 鍒锋柊锟?10s锛岄槻姝婚攣

    // 杈呭姪鍑芥暟锛氫粠 MinIO 鍥炲～ headers/data
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

    // 寮傛鍒锋柊鍑芥暟锛氭煡 MySQL 锟?鍥炲啓 Redis(data+ts) 锟?閲婃斁锟?
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
            // Not found 锟?cache null marker to prevent penetration
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
            // 1a) Null-cache hit 锟?penetration protection
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
                return grpc::Status::OK;  // 绔嬪嵆杩斿洖锛屼笉绛夊緟鍒锋柊
            }
            if (slog_)
                LOG_ERROR(*slog_, "Get id=" + std::to_string(req->id()) + " ParseFromString FAILED key=" + cache_key);
        } else {
            if (slog_)
                LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " MISS key=" + cache_key);
        }
    }

    // 2) Cold start: Redis MISS 锟?MySQL
    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    SpreadsheetRow row;
    // 鍚岃繛鎺ュ彲鑳界灛鏃朵笉鍙宸叉彁浜ゆ暟鎹紝鐭殏閲嶈瘯
    bool got = false;
    for (int r = 0; r < 3 && db_; ++r) {
        got = db_->GetSpreadsheet(req->id(), req_uid, row);
        if (got) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50 << r));
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
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;
    ScopeTimer timer;
    std::string username = UsernameFromMeta(context);
    int64_t uid = g_rpc_auth_ctx.user_id;
    int page = req->page();
    int page_size = req->page_size();

    // 1) Try Redis cache
    if (TryListCache(resp, uid, page, page_size, redis_, "sheet", slog_)) {
        if (logger_) {
            json p, r;
            r["cache"] = json("redis");
            logger_->Log(username, "SpreadsheetService", "List", p, r, true, timer.elapsedUs());
        }
        return grpc::Status::OK;
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
    PopulateListCache(resp, uid, page, page_size, redis_, "sheet", slog_);

    if (logger_) {
        auto dur = timer.elapsedUs();
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
    if (!g_rpc_auth_ctx.authenticated)
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, g_rpc_auth_ctx.user_id, vu_user, vu_role))
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
        if (!db_->GetSpreadsheetOwner(req->id(), owner_uid, &version) || owner_uid != g_rpc_auth_ctx.user_id) {
            resp->set_success(false);
            SET_ERROR(resp, "Not found or permission denied", rpc_error::NOT_FOUND);
            return grpc::Status::OK;
        }

        bool ok = db_->UpdateSpreadsheet(req->id(), g_rpc_auth_ctx.user_id, req->name(), req->description(),
                                         req->headers_json(), req->data_json(), version);

        if (ok) {
            if (redis_ && redis_->IsConnected()) {
                redis_->DeleteKey("u:" + std::to_string(g_rpc_auth_ctx.user_id) + ":sheet:" + std::to_string(req->id()));
                redis_->DeleteKey("u:" + std::to_string(g_rpc_auth_ctx.user_id) + ":sheet:" + std::to_string(req->id()) +
                                  ":ts");
                redis_->Increment(SheetVersionKey(g_rpc_auth_ctx.user_id));
                if (slog_)
                    LOG_DEBUG(*slog_, "Update id=" + std::to_string(req->id()) +
                                          " INVALIDATED + INCR version uid=" + std::to_string(g_rpc_auth_ctx.user_id));
            }

            {
                nlohmann::json ev;
                ev["type"] = "sheet.updated";
                ev["id"] = req->id();
                ev["user_id"] = g_rpc_auth_ctx.user_id;
                ev["name"] = req->name();
                ev["description"] = req->description();
                if (rabbit_)
                    rabbit_->Publish("rpc.events", "sheet.updated", ev.dump());
                if (db_)
                    db_->InsertOutbox(g_rpc_auth_ctx.user_id, "sheet.updated", ev.dump());
            }

            // Invalidate caches via outbox
            InvalidateCaches(db_, g_rpc_auth_ctx.user_id, {
                SheetCacheKey(g_rpc_auth_ctx.user_id, req->id()),
                SheetVersionKey(g_rpc_auth_ctx.user_id)
            });

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
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;
    return HandleDelete(resp, req->id(), g_rpc_auth_ctx.user_id, UsernameFromMeta(context),
        db_, rabbit_, redis_, logger_, slog_,
        [&](auto id, auto &owner) { return db_->GetSpreadsheetOwner(id, owner); },
        [&](auto id, auto uid) { return db_->DeleteSpreadsheet(id, uid); },
        {"sheet.deleted", "id", "SpreadsheetService",
         SheetCacheKey(g_rpc_auth_ctx.user_id, req->id()),
         SheetVersionKey(g_rpc_auth_ctx.user_id)});
}
