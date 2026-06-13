#include "file_service_impl.h"
#include "auth_interceptor.h"
#include "database.h"
#include "redis_client.h"
#include "l1_cache.h"
#include "call_logger.h"
#include "system_logger.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>



static std::string UsernameFromMeta(grpc::ServerContext* ctx) {
    auto it = ctx->client_metadata().find("username");
    if (it != ctx->client_metadata().end())
        return std::string(it->second.data(), it->second.length());
    return "";
}

bool FileServiceImpl::ValidateCaller(grpc::ServerContext* ctx, int64_t user_id,
                                      std::string& out_username, std::string& out_role) const {
    if (!auth_stub_) return true;
    std::string token;
    auto it = ctx->client_metadata().find("authorization");
    if (it != ctx->client_metadata().end()) {
        std::string val(it->second.data(), it->second.length());
        const std::string prefix = "Bearer ";
        if (val.rfind(prefix, 0) == 0) token = val.substr(prefix.size());
        else token = val;
    }
    if (token.empty()) return false;
    rpc::ValidateUserRequest vu_req;
    rpc::ValidateUserResponse vu_resp;
    vu_req.set_token(token);
    vu_req.set_user_id(user_id);
    grpc::ClientContext vu_ctx;
    vu_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto st = auth_stub_->ValidateUser(&vu_ctx, vu_req, &vu_resp);
    if (!st.ok() || !vu_resp.valid()) return false;
    out_username = vu_resp.username();
    out_role = vu_resp.role();
    return true;
}

static std::string FileVersionKey(int64_t user_id) {
    return "u:" + std::to_string(user_id) + ":files:version";
}
static std::string FileListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    std::string key = "u:" + std::to_string(user_id) + ":files:v" + std::to_string(version);
    if (page_size > 0) key += ":p" + std::to_string(page) + ":ps" + std::to_string(page_size);
    return key;
}

grpc::Status FileServiceImpl::CreateFile(grpc::ServerContext* context,
                                         const rpc::CreateFileRequest* req,
                                         rpc::CreateFileResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    // MinIO 优先：上传内容 → 元数据写 MySQL（storage_path）；回退 LONGBLOB
    int64_t id = 0;
    std::string storage_key;
    if (minio_ && minio_->IsConfigured() && !req->file_content().empty()) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        storage_key = std::to_string(req->user_id()) + "/" + std::to_string(us);
        if (!minio_->PutObject(storage_key, req->file_content(), req->mime_type())) {
            resp->set_success(false);
            resp->set_error("MinIO upload failed");
            return grpc::Status::OK;
        }
    }

    bool ok = db_->CreateFile(req->user_id(), username, req->original_name(),
                              req->size(), req->mime_type(),
                              storage_key, id, req->idempotency_key());

    if (!ok) {
        if (!storage_key.empty()) minio_->DeleteObject(storage_key);
        resp->set_success(false);
        resp->set_error("Failed to create file record");
        return grpc::Status::OK;
    }

    // 回退：MinIO 不可用时文件内容存 MySQL LONGBLOB
    if (!req->file_content().empty() && storage_key.empty()) {
        db_->UpdateFileContent(id, req->file_content());
    }

    if (redis_ && redis_->IsConnected()) {
        redis_->Increment(FileVersionKey(req->user_id()));
    }

    resp->set_success(true);
    resp->set_id(id);

    // 发布 RabbitMQ 事件 + outbox 兜底
    {
        nlohmann::json ev;
        ev["type"] = "file.uploaded";
        ev["file_id"] = id;
        ev["user_id"] = req->user_id();
        ev["original_name"] = req->original_name();
        ev["mime_type"] = req->mime_type();
        ev["size"] = req->size();
        if (!storage_key.empty()) ev["object_key"] = storage_key;
        if (rabbit_) rabbit_->Publish("rpc.events", "file.uploaded", ev.dump());
        if (db_) db_->InsertOutbox(req->user_id(), "file.uploaded", ev.dump());
    }

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["name"] = json(req->original_name());
        r["id"] = json(static_cast<double>(id));
        logger_->Log(username, "FileService", "Create", p, r, true, dur);
    }
    if (slog_) LOG_INFO(*slog_, "Created id=" + std::to_string(id) + " '" + req->original_name() + "' by " + username + " (uid=" + std::to_string(req->user_id()) + ") storage=mysql-blob");
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::GetFile(grpc::ServerContext* context,
                                      const rpc::GetFileRequest* req,
                                      rpc::GetFileResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");

    if (req->user_id() <= 0) {
        resp->set_success(false);
        resp->set_error("Not found");
        return grpc::Status::OK;
    }
    int64_t owner_uid = 0;
    if (!db_ || !db_->GetFileOwner(req->id(), owner_uid) || owner_uid != req->user_id()) {
        resp->set_success(false);
        resp->set_error("Not found");
        return grpc::Status::OK;
    }

    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    int64_t req_uid = req->user_id();
    const std::string cache_key = "u:" + std::to_string(req_uid) + ":file:" + std::to_string(req->id());
    const std::string ts_key    = cache_key + ":ts";
    const std::string lock_key  = "lock:u:" + std::to_string(req_uid) + ":file:" + std::to_string(req->id());
    const std::string kNullMarker = "__NULL__";
    const int  LOGICAL_TTL  = 300;
    const int  PHYSICAL_TTL = 3600;
    const int  NULL_TTL     = 60;
    const int  LOCK_TTL     = 10;

    // 异步刷新函数
    auto async_refresh = [this, kNullMarker](int64_t id, int64_t uid, std::string ck, std::string tk, std::string lk) {
        FileRow row;
        if (db_ && db_->GetFile(id, uid, row)) {
            rpc::GetFileResponse fresh;
            auto* f = fresh.mutable_file();
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
                    redis_->SetJSON(tk, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                    if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(id) + " ASYNC-REFRESHED key=" + ck);
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
                resp->set_success(false); resp->set_error("Not found");
                resp->set_cache_source("l1"); return grpc::Status::OK;
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
                resp->set_error("Not found");
                resp->set_cache_source("redis");
                return grpc::Status::OK;
            }
            if (resp->ParseFromString(cached)) {
                resp->set_cache_source("redis");
                if (l1_) l1_->Set(cache_key, cached);  // L1 backfill
                // Check logical expiration
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
                if (need_refresh && redis_->SetNX(lock_key, "1", LOCK_TTL)) {
                    if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " STALE async refresh triggered");
                    std::thread([this, async_refresh, id=req->id(), uid=req_uid, ck=cache_key, tk=ts_key, lk=lock_key]() {
                        std::atomic<bool> done{false};
                        std::thread wd([this, &done, &lk]() {
                            while (!done) {
                                std::this_thread::sleep_for(std::chrono::seconds(5));
                                if (!done && redis_) redis_->ExpireKey(lk, 10);
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
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    FileRow row;
    if (!db_->GetFile(req->id(), req_uid, row)) {
        if (redis_ && redis_->IsConnected()) {
            redis_->SetJSON(cache_key, kNullMarker, RedisClient::JitteredTTL(NULL_TTL, 30));
            redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(NULL_TTL, 30));
        }
        resp->set_success(false);
        resp->set_error("Not found");
        return grpc::Status::OK;
    }

    auto* f = resp->mutable_file();
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
                redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), RedisClient::JitteredTTL(PHYSICAL_TTL, 600));
                if (l1_) l1_->Set(cache_key, serialized);  // L1 backfill
                if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(req->id()) + " POPULATED key=" + cache_key);
            }
        }
    }

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        r["cache"] = json("mysql");
        logger_->Log(username, "FileService", "Get", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::ListFiles(grpc::ServerContext* context,
                                         const rpc::ListFilesRequest* req,
                                         rpc::ListFilesResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);
    int64_t uid = req->user_id();
    int page      = req->page();
    int page_size = req->page_size();

    std::string version_key = FileVersionKey(uid);

    // 1) Try Redis cache
    if (redis_ && redis_->IsConnected()) {
        int64_t version = redis_->GetInt(version_key);
        std::string cache_key = FileListCacheKey(uid, version, page, page_size);
        std::string cached;
        if (redis_->GetJSON(cache_key, cached)) {
            if (resp->ParseFromString(cached)) {
                resp->set_cache_source("redis");
                return grpc::Status::OK;
            }
        }
    }

    // 2) Fallback to MySQL
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    std::vector<FileRow> files;
    int total = 0;
    if (!db_->ListFiles(uid, files, total, page, page_size)) {
        resp->set_success(false);
        resp->set_error("Query failed");
        return grpc::Status::OK;
    }

    for (const auto& row : files) {
        auto* f = resp->add_files();
        f->set_id(row.id);
        f->set_username(row.username);
        f->set_original_name(row.original_name);
        f->set_size(row.size);
        f->set_mime_type(row.mime_type);
        f->set_created_at(row.created_at);
    }
    resp->set_total(total);
    resp->set_success(true);
    resp->set_cache_source("mysql");

    // 3) Populate Redis cache
    if (redis_ && redis_->IsConnected()) {
        int64_t version = redis_->GetInt(version_key);
        std::string cache_key = FileListCacheKey(uid, version, page, page_size);
        std::string serialized;
        if (resp->SerializeToString(&serialized)) {
            redis_->SetJSON(cache_key, serialized, 120);
        }
    }

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        r["total"] = json(static_cast<double>(total));
        r["cache"] = json("mysql");
        logger_->Log(username, "FileService", "List", p, r, true, dur);
    }
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::DeleteFile(grpc::ServerContext* context,
                                          const rpc::DeleteFileRequest* req,
                                          rpc::DeleteFileResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    std::string vu_user, vu_role;
    if (auth_stub_ && !ValidateCaller(context, req->user_id(), vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    int64_t owner_uid = 0;
    if (!db_->GetFileOwner(req->id(), owner_uid) || owner_uid != req->user_id()) {
        resp->set_success(false);
        resp->set_error("Not found or permission denied");
        return grpc::Status::OK;
    }

    bool ok = db_->DeleteFile(req->id(), req->user_id());
    if (!ok) {
        resp->set_success(false);
        resp->set_error("Failed to delete");
        return grpc::Status::OK;
    }

    resp->set_success(true);

    {
        nlohmann::json ev;
        ev["type"] = "file.deleted";
        ev["file_id"] = req->id();
        ev["user_id"] = req->user_id();
        if (rabbit_) rabbit_->Publish("rpc.events", "file.deleted", ev.dump());
        if (db_) db_->InsertOutbox(req->user_id(), "file.deleted", ev.dump());
    }

    if (redis_ && redis_->IsConnected()) {
        redis_->DeleteKey("u:" + std::to_string(req->user_id()) + ":file:" + std::to_string(req->id()));
        redis_->DeleteKey("u:" + std::to_string(req->user_id()) + ":file:" + std::to_string(req->id()) + ":ts");
        redis_->Increment(FileVersionKey(req->user_id()));
    }

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["id"] = json(static_cast<double>(req->id()));
        logger_->Log(username, "FileService", "Delete", p, r, true, dur);
    }
    if (slog_) LOG_INFO(*slog_, "Deleted id=" + std::to_string(req->id()) + " by " + username);
    return grpc::Status::OK;
}
