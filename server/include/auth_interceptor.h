#pragma once
#include <string>
#include <memory>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>

struct AuthContext {
    std::string username;
    int64_t user_id = -1;
    bool authenticated = false;
};

class AuthInterceptor {
public:
    AuthInterceptor(const std::string& jwt_secret);

    // Extract and verify JWT from gRPC metadata. Returns authenticated user info.
    AuthContext Authenticate(grpc::ServerContext* ctx);

private:
    std::string jwt_secret_;
};
