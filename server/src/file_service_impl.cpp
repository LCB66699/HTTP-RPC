#include "file_service_impl.h"
#include "auth_interceptor.h"
#include "database.h"
#include "redis_client.h"
#include "call_logger.h"
#include "system_logger.h"
#include <cstdio>
#include <chrono>
#include <thread>

using json = rpc_json::Value;

static std::string UsernameFromMeta(grpc::ServerContext* ctx) {
    auto it = ctx->client_metadata().find("username");
    if (it != ctx->client_metadata().end())
        return std::string(it->second.data(), it->second.length());
    return "";
}

static std::string FileVersionKey(int64_t user_id) {
    return "user:" + std::to_string(user_id) + ":files:version";
}
static std::string FileListCacheKey(int64_t user_id, int64_t version, int page, int page_size) {
    std::string key = "user:" + std::to_string(user_id) + ":files:v" + std::to_string(version);
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
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        return grpc::Status::OK;
    }

    // 文件内容强制走 MinIO，禁止降级 MySQL LONGBLOB（防 Buffer Pool 污染）
    std::string storage_key;
    if (!req->file_content().empty()) {
        if (!minio_ || !minio_->IsConfigured()) {
            resp->set_success(false);
            resp->set_error("Object storage not available — upload rejected");
            return grpc::Status::OK;
        }

        std::string ikey = req->idempotency_key();
        if (ikey.empty()) ikey = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            start.time_since_epoch()).count());
        storage_key = std::to_string(req->user_id()) + "/" + ikey + "/" + req->original_name();

        if (!minio_->PutObject(storage_key, req->file_content(), req->mime_type())) {
            resp->set_success(false);
            resp->set_error("Object storage upload failed");
            return grpc::Status::OK;
        }
        if (slog_) LOG_INFO(*slog_, "Uploaded to MinIO key='" + storage_key + "'");
    }

    int64_t id = 0;
    bool ok = db_->CreateFile(req->user_id(), username, req->original_name(),
                              req->size(), req->mime_type(),
                              storage_key, id, req->idempotency_key());

    if (!ok) {
        resp->set_success(false);
        resp->set_error("Failed to create file record");
        return grpc::Status::OK;
    }

    if (redis_ && redis_->IsConnected()) {
        redis_->Increment(FileVersionKey(req->user_id()));
    }

    resp->set_success(true);
    resp->set_id(id);

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p, r;
        p["name"] = json(req->original_name());
        r["id"] = json(static_cast<double>(id));
        logger_->Log(username, "FileService", "Create", p, r, true, dur);
    }
    if (slog_) LOG_INFO(*slog_, "Created id=" + std::to_string(id) + " '" + req->original_name() + "' by " + username + " (uid=" + std::to_string(req->user_id()) + ") storage=" + (storage_key.empty() ? "mysql-blob" : "minio"));
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::GetFile(grpc::ServerContext* context,
                                      const rpc::GetFileRequest* req,
                                      rpc::GetFileResponse* resp) {
    if (auth_) {
        AuthContext ac = auth_->Authenticate(context);
        if (!ac.authenticated) return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    }
    auto start = std::chrono::high_resolution_clock::now();
    std::string username = UsernameFromMeta(context);

    // Bump key so old entries (presigned-only, no body) are never reused.
    const std::string cache_key = "file:body:" + std::to_string(req->id());
    const std::string ts_key    = cache_key + ":ts";
    const std::string lock_key  = "lock:file:body:" + std::to_string(req->id());
    const std::string kNullMarker = "__NULL__";
    const int  LOGICAL_TTL  = 300;
    const int  PHYSICAL_TTL = 3600;
    const int  NULL_TTL     = 60;
    const int  LOCK_TTL     = 10;

    int64_t req_uid = req->user_id();

    // 异步刷新函数
    auto async_refresh = [this, kNullMarker, NULL_TTL, PHYSICAL_TTL](int64_t id, int64_t uid, std::string ck, std::string tk, std::string lk) {
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
            // Read file content
            if (!row.storage_path.empty() && minio_ && minio_->IsConfigured()) {
                std::string bytes;
                if (minio_->GetObject(row.storage_path, bytes)) {
                    fresh.set_file_content(std::move(bytes));
                }
            } else {
                fresh.set_file_content(row.file_content);
            }
            // Cache only if < 1MB
            if (fresh.file_content().size() < 1024 * 1024) {
                std::string ser;
                if (fresh.SerializeToString(&ser) && redis_ && redis_->IsConnected()) {
                    redis_->SetJSON(ck, ser, PHYSICAL_TTL);
                    redis_->SetJSON(tk, std::to_string(std::time(nullptr)), PHYSICAL_TTL);
                    if (slog_) LOG_DEBUG(*slog_, "Get id=" + std::to_string(id) + " ASYNC-REFRESHED key=" + ck);
                }
            }
        } else if (db_ && redis_ && redis_->IsConnected()) {
            redis_->SetJSON(ck, kNullMarker, NULL_TTL);
            redis_->SetJSON(tk, std::to_string(std::time(nullptr)), NULL_TTL);
        }
        if (redis_ && redis_->IsConnected()) {
            redis_->DeleteKey(lk);
        }
    };

    // 1) Try Redis cache
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
                    std::thread(async_refresh, req->id(), req_uid, cache_key, ts_key, lock_key).detach();
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
            redis_->SetJSON(cache_key, kNullMarker, NULL_TTL);
            redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), NULL_TTL);
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

    if (!row.storage_path.empty() && minio_ && minio_->IsConfigured()) {
        std::string bytes;
        if (!minio_->GetObject(row.storage_path, bytes)) {
            resp->set_success(false);
            resp->set_error("Object storage read failed");
            return grpc::Status::OK;
        }
        resp->set_file_content(std::move(bytes));
    } else {
        resp->set_file_content(row.file_content);
    }

    // 3) Populate cache (skip if >1MB)
    if (redis_ && redis_->IsConnected()) {
        if (resp->file_content().size() < 1024 * 1024) {
            std::string serialized;
            if (resp->SerializeToString(&serialized)) {
                redis_->SetJSON(cache_key, serialized, PHYSICAL_TTL);
                redis_->SetJSON(ts_key, std::to_string(std::time(nullptr)), PHYSICAL_TTL);
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

    // Delete MinIO object before removing the MySQL row, so we can abort if
    // object storage is unreachable (data stays consistent).
    std::string storage_path;
    db_->GetFileStoragePath(req->id(), storage_path);
    if (!storage_path.empty() && minio_ && minio_->IsConfigured()) {
        if (!minio_->DeleteObject(storage_path)) {
            if (slog_) LOG_WARN(*slog_, "Delete id=" + std::to_string(req->id()) + " MinIO DeleteObject failed for key=" + storage_path);
        }
    }

    bool ok = db_->DeleteFile(req->id());
    if (!ok) {
        resp->set_success(false);
        resp->set_error("Failed to delete");
        return grpc::Status::OK;
    }

    resp->set_success(true);

    if (redis_ && redis_->IsConnected()) {
        redis_->DeleteKey("file:" + std::to_string(req->id()));
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
