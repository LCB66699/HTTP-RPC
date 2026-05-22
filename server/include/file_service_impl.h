#pragma once
#include <string>
#include <grpcpp/grpcpp.h>
#include "generated/rpc_file.grpc.pb.h"
#include "generated/rpc_file.pb.h"
#include "minio_client.h"

class Database;
class RedisClient;
class CallLogger;
class SystemLogger;

class AuthInterceptor;

class FileServiceImpl final : public rpc::FileService::Service {
public:
    void SetDatabase(Database* db) { db_ = db; }
    void SetRedis(RedisClient* redis) { redis_ = redis; }
    void SetLogger(CallLogger* logger) { logger_ = logger; }
    void SetSysLog(SystemLogger* slog) { slog_ = slog; }
    // Optional: when set, file bodies go to MinIO instead of MySQL LONGBLOB.
    void SetMinio(minio::Client* mc) { minio_ = mc; }
    void SetAuthInterceptor(AuthInterceptor* interceptor) { auth_ = interceptor; }

    grpc::Status CreateFile(grpc::ServerContext* ctx,
                            const rpc::CreateFileRequest* req,
                            rpc::CreateFileResponse* resp) override;
    grpc::Status GetFile(grpc::ServerContext* ctx,
                         const rpc::GetFileRequest* req,
                         rpc::GetFileResponse* resp) override;
    grpc::Status ListFiles(grpc::ServerContext* ctx,
                           const rpc::ListFilesRequest* req,
                           rpc::ListFilesResponse* resp) override;
    grpc::Status DeleteFile(grpc::ServerContext* ctx,
                            const rpc::DeleteFileRequest* req,
                            rpc::DeleteFileResponse* resp) override;

private:
    Database*       db_    = nullptr;
    RedisClient*    redis_ = nullptr;
    CallLogger*     logger_ = nullptr;
    SystemLogger*   slog_  = nullptr;
    minio::Client*  minio_ = nullptr;
    AuthInterceptor* auth_ = nullptr;
};
