#pragma once
#include "generated/rpc_auth.grpc.pb.h"
#include "generated/rpc_auth.pb.h"
#include "generated/rpc_spreadsheet.grpc.pb.h"
#include "generated/rpc_spreadsheet.pb.h"
#include "minio_client.h"
#include "rabbit_publisher.h"
#include <grpcpp/grpcpp.h>
#include <string>

class ShardedDatabase;
class RedisClient;
class CallLogger;
class SystemLogger;
class L1Cache;

class AuthInterceptor;

class SpreadsheetServiceImpl final : public rpc::SpreadsheetService::Service {
public:
  void SetDatabase(ShardedDatabase *db) { db_ = db; }
  void SetRedis(RedisClient *redis) { redis_ = redis; }
  void SetL1Cache(L1Cache *cache) { l1_ = cache; }
  void SetLogger(CallLogger *logger) { logger_ = logger; }
  void SetSysLog(SystemLogger *slog) { slog_ = slog; }
  void SetAuthInterceptor(AuthInterceptor *interceptor) { auth_ = interceptor; }
  void SetRabbitMQ(RabbitPublisher *rb) { rabbit_ = rb; }
  void SetMinio(minio::Client *mc) { minio_ = mc; }
  void SetAuthChannel(std::shared_ptr<grpc::Channel> ch) {
    auth_stub_ = rpc::AuthService::NewStub(ch);
  }

  // 从 gRPC metadata 提取 token 并调用 Auth.ValidateUser
  bool ValidateCaller(grpc::ServerContext *ctx, int64_t user_id,
                      std::string &out_username, std::string &out_role) const;

  grpc::Status CreateSpreadsheet(grpc::ServerContext *ctx,
                                 const rpc::CreateSpreadsheetRequest *req,
                                 rpc::CreateSpreadsheetResponse *resp) override;
  grpc::Status GetSpreadsheet(grpc::ServerContext *ctx,
                              const rpc::GetSpreadsheetRequest *req,
                              rpc::GetSpreadsheetResponse *resp) override;
  grpc::Status ListSpreadsheets(grpc::ServerContext *ctx,
                                const rpc::ListSpreadsheetsRequest *req,
                                rpc::ListSpreadsheetsResponse *resp) override;
  grpc::Status UpdateSpreadsheet(grpc::ServerContext *ctx,
                                 const rpc::UpdateSpreadsheetRequest *req,
                                 rpc::UpdateSpreadsheetResponse *resp) override;
  grpc::Status DeleteSpreadsheet(grpc::ServerContext *ctx,
                                 const rpc::DeleteSpreadsheetRequest *req,
                                 rpc::DeleteSpreadsheetResponse *resp) override;

private:
  ShardedDatabase *db_ = nullptr;
  RedisClient *redis_ = nullptr;
  L1Cache *l1_ = nullptr;
  CallLogger *logger_ = nullptr;
  SystemLogger *slog_ = nullptr;
  AuthInterceptor *auth_ = nullptr;
  std::unique_ptr<rpc::AuthService::Stub> auth_stub_;
  RabbitPublisher *rabbit_ = nullptr;
  minio::Client *minio_ = nullptr;
};
