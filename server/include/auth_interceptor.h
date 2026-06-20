#pragma once
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_context.h>

#include <memory>
#include <string>

struct AuthContext {
    std::string username;
    int64_t user_id = -1;
    bool authenticated = false;
};

class AuthInterceptor {
   public:
    AuthInterceptor(const std::string &jwt_secret);

    // Extract and verify JWT from gRPC metadata. Returns authenticated user info.
    AuthContext Authenticate(grpc::ServerContext *ctx);

    // Verify a raw JWT token string and return authenticated user info.
    // Shared by gRPC interceptor (which can't access ServerContext in gRPC 1.51).
    static AuthContext FromToken(const std::string &token, const std::string &jwt_secret);

   private:
    std::string jwt_secret_;
};
