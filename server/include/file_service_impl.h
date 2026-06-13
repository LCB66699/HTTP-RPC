#pragma once
#include <grpcpp/grpcpp.h>

#include <string>

#include "generated/rpc_auth.grpc.pb.h"
#include "generated/rpc_auth.pb.h"
#include "generated/rpc_file.grpc.pb.h"
#include "generated/rpc_file.pb.h"
#include "minio_client.h"
#include "rabbit_publisher.h"

class ShardedDatabase;
class RedisClient;
class L1Cache;
class CallLogger;
class SystemLogger;

class AuthInterceptor;

class FileServiceImpl final : public rpc::FileService::Service {
   public:
    void SetDatabase(ShardedDatabase *db) { db_ = db; }
    void SetRedis(RedisClient *redis) { redis_ = redis; }
    void SetL1Cache(L1Cache *cache) { l1_ = cache; }
    void SetLogger(CallLogger *logger) { logger_ = logger; }
    void SetSysLog(SystemLogger *slog) { slog_ = slog; }
    void SetMinio(minio::Client *mc) { minio_ = mc; }
    void SetAuthInterceptor(AuthInterceptor *interceptor) { auth_ = interceptor; }
    void SetRabbitMQ(RabbitPublisher *rb) { rabbit_ = rb; }
    void SetAuthChannel(std::shared_ptr<grpc::Channel> ch) { auth_stub_ = rpc::AuthService::NewStub(ch); }

    bool ValidateCaller(grpc::ServerContext *ctx, int64_t user_id, std::string &out_username,
                        std::string &out_role) const;

    grpc::Status CreateFile(grpc::ServerContext *ctx, const rpc::CreateFileRequest *req,
                            rpc::CreateFileResponse *resp) override;
    grpc::Status GetFile(grpc::ServerContext *ctx, const rpc::GetFileRequest *req, rpc::GetFileResponse *resp) override;
    grpc::Status ListFiles(grpc::ServerContext *ctx, const rpc::ListFilesRequest *req,
                           rpc::ListFilesResponse *resp) override;
    grpc::Status DeleteFile(grpc::ServerContext *ctx, const rpc::DeleteFileRequest *req,
                            rpc::DeleteFileResponse *resp) override;

   private:
    ShardedDatabase *db_ = nullptr;
    RedisClient *redis_ = nullptr;
    L1Cache *l1_ = nullptr;
    CallLogger *logger_ = nullptr;
    SystemLogger *slog_ = nullptr;
    minio::Client *minio_ = nullptr;
    AuthInterceptor *auth_ = nullptr;
    std::unique_ptr<rpc::AuthService::Stub> auth_stub_;
    RabbitPublisher *rabbit_ = nullptr;
};
