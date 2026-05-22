#include "auth_interceptor.h"
#include "jwt.h"
#include <cstdio>
#include <ctime>

AuthInterceptor::AuthInterceptor(const std::string& jwt_secret) : jwt_secret_(jwt_secret) {}

AuthContext AuthInterceptor::Authenticate(grpc::ServerContext* ctx) {
    AuthContext auth;
    const auto& metadata = ctx->client_metadata();

    // Check for authorization header
    auto it = metadata.find("authorization");
    if (it == metadata.end()) {
        // Also check grpcgateway-authorization (grpc-gateway forwarded)
        it = metadata.find("grpcgateway-authorization");
    }
    if (it == metadata.end()) {
        fprintf(stderr, "[Auth] Missing authorization header\n");
        return auth;
    }

    std::string val(it->second.data(), it->second.size());
    const std::string prefix = "Bearer ";
    if (val.size() <= prefix.size() || val.substr(0, prefix.size()) != prefix) {
        fprintf(stderr, "[Auth] Invalid authorization format\n");
        return auth;
    }

    std::string token = val.substr(prefix.size());
    std::string payload;
    if (!jwt::verify(token, jwt_secret_, payload)) {
        fprintf(stderr, "[Auth] JWT verification failed\n");
        return auth;
    }

    // 检查 JWT 过期时间（exp 字段，Unix 时间戳）
    auto exp_pos = payload.find("\"exp\"");
    if (exp_pos != std::string::npos) {
        auto colon = payload.find(':', exp_pos + 5);
        if (colon != std::string::npos) {
            size_t num_start = colon + 1;
            while (num_start < payload.size() && (payload[num_start] == ' ' || payload[num_start] == '\t')) num_start++;
            size_t num_end = num_start;
            while (num_end < payload.size() && payload[num_end] >= '0' && payload[num_end] <= '9') num_end++;
            if (num_end > num_start) {
                long exp = std::stol(payload.substr(num_start, num_end - num_start));
                if (exp < (long)std::time(nullptr)) {
                    fprintf(stderr, "[Auth] JWT expired\n");
                    return auth;
                }
            }
        }
    } else {
        fprintf(stderr, "[Auth] JWT missing exp claim\n");
        return auth;
    }

    // Extract username from payload: {"username":"...","exp":...}
    auto us = payload.find("\"username\"");
    if (us == std::string::npos) return auth;
    auto col = payload.find(':', us + 10);
    if (col == std::string::npos) return auth;
    auto start = payload.find('"', col + 1);
    if (start == std::string::npos) return auth;
    auto end = payload.find('"', start + 1);
    if (end == std::string::npos) return auth;

    auth.username = payload.substr(start + 1, end - start - 1);

    // 提取 uid
    auto uid_pos = payload.find("\"uid\"");
    if (uid_pos != std::string::npos) {
        auto colon2 = payload.find(':', uid_pos + 5);
        if (colon2 != std::string::npos) {
            size_t num_start = colon2 + 1;
            while (num_start < payload.size() &&
                   (payload[num_start] == ' ' || payload[num_start] == '\t')) num_start++;
            size_t num_end = num_start;
            while (num_end < payload.size() &&
                   payload[num_end] >= '0' && payload[num_end] <= '9') num_end++;
            if (num_end > num_start)
                auth.user_id = std::stoll(payload.substr(num_start, num_end - num_start));
        }
    }

    auth.authenticated = true;
    return auth;
}
