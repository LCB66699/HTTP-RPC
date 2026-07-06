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

    // 通过熔断器调用 Auth 服务
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

    int64_t id = 0;
    if (slog_) LOG_DEBUG(*slog_, "Create req->user_id=" + std::to_string(g_rpc_auth_ctx.user_id));
    bool ok = db_->CreateSpreadsheet(g_rpc_auth_ctx.user_id, username, req->name(), req->description(), req->headers_json(),
                                     req->data_json(), id, req->idempotency_key());

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

    // Store cells/headers in MongoDB
    if (mongo_) {
        if (!mongo_->UpsertSheetCells(id, g_rpc_auth_ctx.user_id, req->headers_json(), req->data_json())) {
            db_->DeleteSpreadsheet(id, g_rpc_auth_ctx.user_id);
            resp->set_success(false);
            SET_ERROR(resp, "MongoDB write failed", rpc_error::INTERNAL);
            return grpc::Status::OK;
        }
    }

    nlohmann::json extra;
    extra["name"] = req->name();
    extra["headers"] = nlohmann::json::parse(req->headers_json().empty() ? "[]" : req->headers_json());
    extra["cells"] = nlohmann::json::parse(req->data_json().empty() ? "[]" : req->data_json());
    FinishCreate(resp, id, g_rpc_auth_ctx.user_id, username, db_, rabbit_, redis_, logger_, slog_,
        {"sheet.created", "id", "SpreadsheetService", SheetVersionKey(g_rpc_auth_ctx.user_id)},
        std::move(extra), timer.elapsedUs(), l1_);
    return grpc::Status::OK;
}

grpc::Status SpreadsheetServiceImpl::GetSpreadsheet(grpc::ServerContext *context, const rpc::GetSpreadsheetRequest *req,
                                                    rpc::GetSpreadsheetResponse *resp) {
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;
    int64_t req_uid = g_rpc_auth_ctx.user_id;
    bool has_access = CheckOwnerWithRetry(req->id(), req_uid, db_,
            [&](auto id, auto &uid) { return db_->GetSpreadsheetOwner(id, uid); }, slog_);
    if (!has_access && sharing_checker_) {
        std::string perm;
        has_access = sharing_checker_(req_uid, "sheet", req->id(), perm);
    }
    if (!has_access && sharing_stub_) {
        grpc::ClientContext sctx;
        sctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(1500));
        rpc::CheckAccessRequest ca;
        ca.set_user_id(req_uid);
        ca.set_resource_type("sheet");
        ca.set_resource_id(req->id());
        rpc::CheckAccessResponse cr;
        has_access = sharing_stub_->CheckAccess(&sctx, ca, &cr).ok() && cr.allowed();
    }
    if (!has_access) {
        int64_t wid = 0;
        if (db_->GetSpreadsheetWorkspaceId(req->id(), wid) && wid > 0) {
            std::string role;
            has_access = db_->GetWorkspaceMemberRole(wid, req_uid, role);
        }
    }
    if (!has_access)
        return WriteResult(resp, HandlerResult<>::Fail("Not found", rpc_error::NOT_FOUND)),
               grpc::Status(grpc::StatusCode::NOT_FOUND, "Not found");
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);
    const std::string cache_key = "u:" + std::to_string(req_uid) + ":sheet:" + std::to_string(req->id());
    const std::string ts_key = cache_key + ":ts";
    const std::string lock_key = "lock:u:" + std::to_string(req_uid) + ":sheet:" + std::to_string(req->id());
    const std::string kNullMarker = "__NULL__";
    const int LOGICAL_TTL = 300;    // 逻辑过期 5min，超时触发异步刷新
    const int PHYSICAL_TTL = 3600;  // 物理过期 1h，防止内存无限增长
    const int NULL_TTL = 60;        // 空值缓存 1min
    const int LOCK_TTL = 10;        // 刷新锁 10s，防止死锁

    // 辅助函数：从 MongoDB 回填 headers/data
    auto fillFromMongo = [this](SpreadsheetRow &row) {
        if (mongo_) {
            mongo_->GetSheetCells(row.id, row.headers_json, row.data_json);
        }
    };

    // 异步刷新函数：查 MySQL -> 回写 Redis(data+ts) -> 释放锁
    auto async_refresh = [this, kNullMarker, &fillFromMongo](int64_t id, int64_t uid, std::string ck, std::string tk,
                                                             std::string lk) {
        SpreadsheetRow row;
        if (db_ && db_->GetSpreadsheet(id, uid, row)) {
            fillFromMongo(row);
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
            // Not found -> cache null marker to prevent penetration
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
                return grpc::Status::OK;  // 立即返回，不等待刷新
            }
            if (slog_)
                LOG_ERROR(*slog_, "Get id=" + std::to_string(req->id()) + " ParseFromString FAILED key=" + cache_key);
        } else {
            if (slog_)
                LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " MISS key=" + cache_key);
        }
    }

    // 2) Cold start: Redis MISS -> MySQL
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
    fillFromMongo(row);
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
    int limit = req->limit() > 0 ? req->limit() : 20;
    std::string cursor = req->cursor();

    // 1) Try Redis cache
    if (TryListCache(resp, uid, limit, cursor, redis_, "sheet", slog_)) {
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

    std::vector<SpreadsheetSummary> sheets;
    std::string next_cursor;
    bool has_more = false;
    if (!db_->ListSpreadsheets(uid, sheets, next_cursor, has_more, limit, cursor)) {
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
    resp->set_next_cursor(next_cursor);
    resp->set_has_more(has_more);
    resp->set_success(true);
    resp->set_cache_source("mysql");

    // 3) Populate Redis cache
    PopulateListCache(resp, uid, limit, cursor, redis_, "sheet", slog_);

    if (logger_) {
        auto dur = timer.elapsedUs();
        json p, r;
        r["has_more"] = json(has_more);
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

    bool skip_owner = false;
    if (sharing_checker_) {
        std::string perm;
        skip_owner = sharing_checker_(g_rpc_auth_ctx.user_id, "sheet", req->id(), perm) && perm == "edit";
    }
    if (!skip_owner && sharing_stub_) {
        grpc::ClientContext sctx;
        sctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(1500));
        rpc::CheckAccessRequest ca;
        ca.set_user_id(g_rpc_auth_ctx.user_id);
        ca.set_resource_type("sheet");
        ca.set_resource_id(req->id());
        rpc::CheckAccessResponse cr;
        if (sharing_stub_->CheckAccess(&sctx, ca, &cr).ok() && cr.allowed() && cr.permission() == "edit")
            skip_owner = true;
    }
    if (!skip_owner) {
        int64_t wid = 0;
        if (db_->GetSpreadsheetWorkspaceId(req->id(), wid) && wid > 0) {
            std::string role;
            if (db_->GetWorkspaceMemberRole(wid, g_rpc_auth_ctx.user_id, role))
                skip_owner = (role == "editor" || role == "admin");
        }
    }

    auto ok = UpdateWithCAS(
        req->id(), g_rpc_auth_ctx.user_id,
        [&](int64_t id, int64_t &owner, int &ver) {
            return db_ && db_->GetSpreadsheetOwner(id, owner, &ver);
        },
        [&](int64_t id, int ver) {
            return db_->UpdateSpreadsheet(id, g_rpc_auth_ctx.user_id,
                req->name(), req->description(),
                req->headers_json(), req->data_json(), ver);
        },
        [&](int latest) { resp->set_latest_version(latest); }, {}, skip_owner);

    if (!ok) {
        if (resp->latest_version() > 0) {
            resp->set_success(false);
            SET_ERROR(resp, "Update conflict", rpc_error::CONFLICT);
        } else {
            resp->set_success(false);
            SET_ERROR(resp, "Not found or permission denied", rpc_error::NOT_FOUND);
        }
        if (logger_) {
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::high_resolution_clock::now() - start)
                           .count();
            json p, r;
            p["id"] = json(static_cast<double>(req->id()));
            r["error"] = json("Update conflict");
            logger_->Log(username, "SpreadsheetService", "Update", p, r, false, dur);
        }
        return grpc::Status::OK;
    }

    // Store cells/headers in MongoDB
    if (mongo_) {
        mongo_->UpsertSheetCells(req->id(), g_rpc_auth_ctx.user_id,
                                 req->headers_json(), req->data_json());
    }

    if (redis_ && redis_->IsConnected()) {
        redis_->DeleteKey("u:" + std::to_string(g_rpc_auth_ctx.user_id) + ":sheet:" + std::to_string(req->id()));
        redis_->DeleteKey("u:" + std::to_string(g_rpc_auth_ctx.user_id) + ":sheet:" + std::to_string(req->id()) +
                          ":ts");
        redis_->Increment(SheetVersionKey(g_rpc_auth_ctx.user_id));
    }

    {
        nlohmann::json ev;
        ev["type"] = "sheet.updated";
        ev["id"] = req->id();
        ev["user_id"] = g_rpc_auth_ctx.user_id;
        ev["name"] = req->name();
        ev["description"] = req->description();
        ev["headers"] = nlohmann::json::parse(req->headers_json().empty() ? "[]" : req->headers_json());
        ev["cells"] = nlohmann::json::parse(req->data_json().empty() ? "[]" : req->data_json());
        if (db_)
            db_->InsertOutbox(g_rpc_auth_ctx.user_id, "sheet.updated", ev.dump());
        if (rabbit_)
            rabbit_->Publish("rpc.events", "sheet.updated", ev.dump());
    }

    InvalidateCaches(db_, g_rpc_auth_ctx.user_id, {
        SheetCacheKey(g_rpc_auth_ctx.user_id, req->id()),
        SheetVersionKey(g_rpc_auth_ctx.user_id)
    }, l1_, redis_);

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
        [&](auto id, auto uid) {
            if (mongo_) mongo_->DeleteSheetCells(id);
            return db_->DeleteSpreadsheet(id, uid);
        },
        {"sheet.deleted", "id", "SpreadsheetService",
         SheetCacheKey(g_rpc_auth_ctx.user_id, req->id()),
         SheetVersionKey(g_rpc_auth_ctx.user_id)}, l1_);
}
