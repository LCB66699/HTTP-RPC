#include "file/file_service_impl.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "shared/base/rpc_interceptor.h"
#include "shared/base/call_logger.h"
#include "shared/cache/circuit_breaker.h"
#include "shared/client/database.h"
#include "shared/base/error_codes.h"
#include "shared/helper/cache_helpers.h"
#include "shared/helper/crud_helpers.h"
#include "shared/helper/handler_helpers.h"
#include "shared/cache/l1_cache.h"
#include "shared/client/redis_client.h"
#include "shared/base/system_logger.h"

bool FileServiceImpl::ValidateCaller(grpc::ServerContext *ctx, int64_t user_id, std::string &out_username,
                                     std::string &out_role) const {
    if (!auth_stub_)
        return true;
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


grpc::Status FileServiceImpl::CreateFile(grpc::ServerContext *context, const rpc::CreateFileRequest *req,
                                         rpc::CreateFileResponse *resp) {
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;
    ScopeTimer timer;
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    // MinIO 优先：上传内容 -> 元数据写 MySQL（storage_path）；回退 LONGBLOB
    int64_t id = 0;
    std::string storage_key;
    if (minio_ && minio_->IsConfigured() && !req->file_content().empty()) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::high_resolution_clock::now().time_since_epoch())
                      .count();
        storage_key = std::to_string(g_rpc_auth_ctx.user_id) + "/" + std::to_string(us);
        if (!minio_->PutObject(storage_key, req->file_content(), req->mime_type())) {
            resp->set_success(false);
            SET_ERROR(resp, "MinIO upload failed", rpc_error::INTERNAL);
            return grpc::Status::OK;
        }
    }

    bool ok = db_->CreateFile(g_rpc_auth_ctx.user_id, username, req->original_name(), req->size(), req->mime_type(),
                              storage_key, id, req->idempotency_key());

    if (!ok) {
        if (!storage_key.empty())
            minio_->DeleteObject(storage_key);
        resp->set_success(false);
        SET_ERROR(resp, "Failed to create file record", rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    // 回退：MinIO 不可用时文件内容存 MySQL LONGBLOB
    // version=0: 新创建文件，无并发冲突，跳过乐观锁检查
    if (!req->file_content().empty() && storage_key.empty()) {
        db_->UpdateFileContent(id, req->file_content());
    }

    nlohmann::json extra;
    extra["original_name"] = req->original_name();
    extra["mime_type"] = req->mime_type();
    extra["size"] = req->size();
    if (!storage_key.empty())
        extra["object_key"] = storage_key;
    FinishCreate(resp, id, g_rpc_auth_ctx.user_id, username, db_, rabbit_, redis_, logger_, slog_,
        {"file.uploaded", "file_id", "FileService", FileVersionKey(g_rpc_auth_ctx.user_id)},
        std::move(extra), timer.elapsedUs(), l1_);
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::GetFile(grpc::ServerContext *context, const rpc::GetFileRequest *req,
                                      rpc::GetFileResponse *resp) {
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;

    if (!CheckOwnerWithRetry(req->id(), g_rpc_auth_ctx.user_id, db_,
            [&](auto id, auto &uid) { return db_->GetFileOwner(id, uid); }, slog_))
        return WriteResult(resp, HandlerResult<>::Fail("Not found", rpc_error::NOT_FOUND)),
               grpc::Status(grpc::StatusCode::NOT_FOUND, "Not found");

    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    int64_t req_uid = g_rpc_auth_ctx.user_id;
    const std::string cache_key = "u:" + std::to_string(req_uid) + ":file:" + std::to_string(req->id());
    const std::string ts_key = cache_key + ":ts";
    const std::string lock_key = "lock:u:" + std::to_string(req_uid) + ":file:" + std::to_string(req->id());
    const std::string kNullMarker = "__NULL__";
    const int LOGICAL_TTL = 300;
    const int PHYSICAL_TTL = 3600;
    const int NULL_TTL = 60;
    const int LOCK_TTL = 10;

    // 异步刷新函数
    auto async_refresh = [this, kNullMarker](int64_t id, int64_t uid, std::string ck, std::string tk, std::string lk) {
        FileRow row;
        if (db_ && db_->GetFile(id, uid, row)) {
            rpc::GetFileResponse fresh;
            auto *f = fresh.mutable_file();
            f->set_id(row.id);
            f->set_username(row.username);
            f->set_original_name(row.original_name);
            f->set_size(row.size);
            f->set_mime_type(row.mime_type);
            f->set_created_at(row.created_at);
            fresh.set_success(true);
            fresh.set_file_content(row.file_content);
            // Cache only if < 1MB
            if (fresh.file_content().size() < 1024 * 1024) {
                std::string ser;
                if (fresh.SerializeToString(&ser) && redis_ && redis_->IsConnected()) {
                    redis_->SetJSON(ck, ser, RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                    redis_->SetJSON(tk, std::to_string(std::time(nullptr)),
                                    RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                    if (slog_)
                        LOG_DEBUG(*slog_, "Get id=" + std::to_string(id) + " ASYNC-REFRESHED key=" + ck);
                }
            }
        } else if (db_ && redis_ && redis_->IsConnected()) {
            redis_->SetJSON(ck, kNullMarker, RedisClient::JitteredTTL(NULL_TTL, 30));
            redis_->SetJSON(tk, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(NULL_TTL, 30));
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
                return grpc::Status::OK;
            }
        }
    }

    // 2) Try Redis cache
    if (redis_ && redis_->IsConnected()) {
        std::string cached;
        if (redis_->GetJSON(cache_key, cached)) {
            if (cached == kNullMarker) {
                resp->set_success(false);
                SET_ERROR(resp, "Not found", rpc_error::NOT_FOUND);
                resp->set_cache_source("redis");
                return grpc::Status::OK;
            }
            if (resp->ParseFromString(cached)) {
                resp->set_cache_source("redis");
                if (l1_)
                    l1_->Set(cache_key, cached);  // L1 backfill
                // Check logical expiration
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
                if (need_refresh && redis_->SetNX(lock_key, "1", LOCK_TTL)) {
                    if (slog_)
                        LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " STALE async refresh triggered");
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
                return grpc::Status::OK;
            }
        }
    }

    // 2) Cold start: MySQL
    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    FileRow row;
    bool got = false;
    for (int r = 0; r < 3 && db_; ++r) {
        got = db_->GetFile(req->id(), req_uid, row);
        if (slog_) LOG_DEBUG(*slog_, "GetFile id=" + std::to_string(req->id()) + " attempt=" + std::to_string(r+1) + " ok=" + std::to_string(got));
        if (got) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50 << r));
    }
    if (!got) {
        if (redis_ && redis_->IsConnected()) {
            redis_->SetJSON(cache_key, kNullMarker, RedisClient::JitteredTTL(NULL_TTL, 30));
            redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(NULL_TTL, 30));
        }
        resp->set_success(false);
        SET_ERROR(resp, "Not found", rpc_error::NOT_FOUND);
        return grpc::Status::OK;
    }

    auto *f = resp->mutable_file();
    f->set_id(row.id);
    f->set_username(row.username);
    f->set_original_name(row.original_name);
    f->set_size(row.size);
    f->set_mime_type(row.mime_type);
    f->set_created_at(row.created_at);
    resp->set_success(true);
    resp->set_cache_source("mysql");

    // MinIO 优先：生成预签名 URL 302 重定向；回退 LONGBLOB
    if (minio_ && minio_->IsConfigured() && !row.storage_path.empty()) {
        std::string url = minio_->PresignedGetUrl(row.storage_path, 3600);
        if (!url.empty()) {
            resp->set_download_url(url);
        } else {
            resp->set_file_content(row.file_content);
        }
    } else {
        resp->set_file_content(row.file_content);
    }

    // 3) Populate cache (skip if >1MB)
    if (redis_ && redis_->IsConnected()) {
        if (resp->file_content().size() < 1024 * 1024) {
            std::string serialized;
            if (resp->SerializeToString(&serialized)) {
                redis_->SetJSON(cache_key, serialized, RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)),
                                RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                if (l1_)
                    l1_->Set(cache_key, serialized);  // L1 backfill
                if (slog_)
                    LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " POPULATED key=" + cache_key);
            }
        }
    }

    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        r["cache"] = json("mysql");
        logger_->Log(username, "FileService", "Get", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::ListFiles(grpc::ServerContext *context, const rpc::ListFilesRequest *req,
                                        rpc::ListFilesResponse *resp) {
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
    if (TryListCache(resp, uid, limit, cursor, redis_, "file", slog_))
        return grpc::Status::OK;

    // 2) Fallback to MySQL
    if (!db_) {
        resp->set_success(false);
        SET_ERROR(resp, "Database not available", rpc_error::UNAVAILABLE);
        return grpc::Status::OK;
    }

    std::vector<FileRow> files;
    std::string next_cursor;
    bool has_more = false;
    if (!db_->ListFiles(uid, files, next_cursor, has_more, limit, cursor)) {
        resp->set_success(false);
        SET_ERROR(resp, "Query failed", rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    for (const auto &row : files) {
        auto *f = resp->add_files();
        f->set_id(row.id);
        f->set_username(row.username);
        f->set_original_name(row.original_name);
        f->set_size(row.size);
        f->set_mime_type(row.mime_type);
        f->set_created_at(row.created_at);
    }
    resp->set_next_cursor(next_cursor);
    resp->set_has_more(has_more);
    resp->set_success(true);
    resp->set_cache_source("mysql");

    // 3) Populate Redis cache
    PopulateListCache(resp, uid, limit, cursor, redis_, "file", slog_);

    if (logger_) {
        auto dur = timer.elapsedUs();
        json p, r;
        r["has_more"] = json(has_more);
        r["cache"] = json("mysql");
        logger_->Log(username, "FileService", "List", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::DeleteFile(grpc::ServerContext *context, const rpc::DeleteFileRequest *req,
                                         rpc::DeleteFileResponse *resp) {
    if (auto fail = requireAuthWith(context, [this](auto ctx, auto uid, auto &vu, auto &vr) {
            return ValidateCaller(ctx, uid, vu, vr);
        }))
        return *fail;
    return HandleDelete(resp, req->id(), g_rpc_auth_ctx.user_id, UsernameFromMeta(context),
        db_, rabbit_, redis_, logger_, slog_,
        [&](auto id, auto &owner) { return db_->GetFileOwner(id, owner); },
        [&](auto id, auto uid) { return db_->DeleteFile(id, uid); },
        {"file.deleted", "file_id", "FileService",
         FileCacheKey(g_rpc_auth_ctx.user_id, req->id()),
         FileVersionKey(g_rpc_auth_ctx.user_id)}, l1_);
}

grpc::Status FileServiceImpl::CreateFolder(grpc::ServerContext *ctx, const rpc::CreateFolderRequest *req,
                                           rpc::CreateFolderResponse *resp) {
    if (!g_rpc_auth_ctx.authenticated)
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "");
    int64_t id = 0;
    bool ok = db_->CreateFolder(g_rpc_auth_ctx.user_id, req->name(), req->parent_folder_id(), id);
    resp->set_success(ok);
    if (ok)
        resp->set_id(id);
    else
        SET_ERROR(resp, "Failed to create folder", rpc_error::INTERNAL);
    return grpc::Status::OK;
}
grpc::Status FileServiceImpl::MoveFile(grpc::ServerContext *ctx, const rpc::MoveFileRequest *req,
                                       rpc::MoveFileResponse *resp) {
    if (!g_rpc_auth_ctx.authenticated)
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "");
    bool ok = db_->MoveFile(req->id(), req->target_folder_id());
    resp->set_success(ok);
    if (!ok)
        SET_ERROR(resp, "Failed to move file", rpc_error::INTERNAL);
    return grpc::Status::OK;
}
grpc::Status FileServiceImpl::BatchDelete(grpc::ServerContext *ctx, const rpc::BatchDeleteRequest *req,
                                          rpc::BatchDeleteResponse *resp) {
    if (!g_rpc_auth_ctx.authenticated)
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "");
    std::vector<int64_t> ids(req->ids().begin(), req->ids().end());
    int count = db_->BatchDeleteFiles(g_rpc_auth_ctx.user_id, ids);
    resp->set_success(count > 0);
    resp->set_deleted_count(count);
    return grpc::Status::OK;
}





