#pragma once
#include <grpcpp/grpcpp.h>

#include <string>

#include "generated/rpc_auth.grpc.pb.h"
#include "generated/rpc_auth.pb.h"
#include "generated/rpc_file.grpc.pb.h"
#include "generated/rpc_file.pb.h"
#include "shared/cache/circuit_breaker.h"
#include "shared/client/minio_client.h"
#include "shared/base/service_interfaces.h"

class L1Cache;
class CallLogger;
class SystemLogger;

class FileServiceImpl final : public rpc::FileService::Service {
   public:
    void SetDatabase(IDatabase *db) { db_ = db; }
    void SetRedis(IRedisClient *redis) { redis_ = redis; }
    void SetL1Cache(L1Cache *cache) { l1_ = cache; }
    void SetLogger(CallLogger *logger) { logger_ = logger; }
    void SetSysLog(SystemLogger *slog) { slog_ = slog; }
    void SetMinio(minio::Client *mc) { minio_ = mc; }
    void SetRabbitMQ(IRabbitPublisher *rb) { rabbit_ = rb; }
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
    grpc::Status CreateFolder(grpc::ServerContext *, const rpc::CreateFolderRequest *,
                              rpc::CreateFolderResponse *) override;
    grpc::Status MoveFile(grpc::ServerContext *, const rpc::MoveFileRequest *, rpc::MoveFileResponse *) override;
    grpc::Status BatchDelete(grpc::ServerContext *, const rpc::BatchDeleteRequest *,
                             rpc::BatchDeleteResponse *) override;

   private:
    IDatabase *db_ = nullptr;
    IRedisClient *redis_ = nullptr;
    L1Cache *l1_ = nullptr;
    CallLogger *logger_ = nullptr;
    SystemLogger *slog_ = nullptr;
    minio::Client *minio_ = nullptr;
    std::unique_ptr<rpc::AuthService::Stub> auth_stub_;
    mutable GrpcCircuitBreaker auth_cb_;
    IRabbitPublisher *rabbit_ = nullptr;
};
