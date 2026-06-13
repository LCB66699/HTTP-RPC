#pragma once
#include <string>
#include <map>
#include <mutex>
#include <grpcpp/grpcpp.h>
#include "generated/rpc_auth.grpc.pb.h"
#include "generated/rpc_auth.pb.h"

class ShardedDatabase;
class CallLogger;
class RedisClient;
class SystemLogger;

class AuthServiceImpl final : public rpc::AuthService::Service {
public:
    explicit AuthServiceImpl(const std::string& jwt_secret, ShardedDatabase* db = nullptr)
        : jwt_secret_(jwt_secret), db_(db) { LoadUsers(); }

    void SetLogger(CallLogger* logger) { logger_ = logger; }
    void SetRedis(RedisClient* redis) { redis_ = redis; }
    void SetSysLog(SystemLogger* slog) { slog_ = slog; }

    grpc::Status Login(grpc::ServerContext* ctx, const rpc::LoginRequest* req,
                       rpc::LoginResponse* resp) override;

    grpc::Status Register(grpc::ServerContext* ctx, const rpc::RegisterRequest* req,
                          rpc::RegisterResponse* resp) override;

    grpc::Status ValidateUser(grpc::ServerContext* ctx, const rpc::ValidateUserRequest* req,
                               rpc::ValidateUserResponse* resp) override;

    grpc::Status RefreshToken(grpc::ServerContext* ctx, const rpc::RefreshTokenRequest* req,
                               rpc::RefreshTokenResponse* resp) override;

    bool IsAdminUser(const std::string& username) const;

private:
    std::string CreateAccessToken(const std::string& username, int64_t uid, const std::string& role) const;
    std::string CreateRefreshToken(const std::string& username, int64_t uid) const;

    std::string jwt_secret_;
    std::map<std::string, std::string> users_;
    std::mutex mtx_;
    ShardedDatabase* db_ = nullptr;
    CallLogger* logger_ = nullptr;
    RedisClient* redis_ = nullptr;
    SystemLogger* slog_ = nullptr;

    void LoadUsers();
    void SaveUsers();
    std::string UsersFilePath() const;
};
