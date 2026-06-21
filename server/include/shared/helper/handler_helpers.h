#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "shared/base/error_codes.h"
#include "shared/base/rpc_interceptor.h"
#include "shared/base/service_interfaces.h"

// ======== HandlerResult ========

template <typename T = void>
struct HandlerResult {
    bool ok = true;
    std::string error;
    int error_code = 0;
    T value{};

    static HandlerResult Success(T v = {}) { return {true, {}, 0, std::move(v)}; }
    static HandlerResult Fail(std::string msg, int code) {
        return {false, std::move(msg), code, T{}};
    }
};

// Write HandlerResult to a protobuf response (must have set_success/set_error/set_error_code).
template <typename Resp>
void WriteResult(Resp *resp, const HandlerResult<> &r) {
    if (r.ok) {
        resp->set_success(true);
    } else {
        resp->set_success(false);
        SET_ERROR(resp, r.error.c_str(), r.error_code);
    }
}

// ======== Auth guard ========

// requireAuth 鈥?returns grpc::Status::OK if auth passes, else UNAUTHENTICATED.
// Caller: if (auto fail = requireAuth()) return *fail;
inline std::optional<grpc::Status> requireAuth() {
    if (!g_rpc_auth_ctx.authenticated)
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing token");
    return std::nullopt;
}

// requireAuthWith 鈥?auth + ValidateCaller via callable.
// Usage:
//   if (auto fail = requireAuthWith(ctx, [this](auto ctx, auto uid) {
//         return ValidateCaller(ctx, uid, vu_user, vu_role);
//       })) return *fail;
template <typename Fn>
std::optional<grpc::Status> requireAuthWith(grpc::ServerContext *ctx, Fn &&validate) {
    if (auto fail = requireAuth())
        return fail;
    std::string vu_user, vu_role;
    if (!validate(ctx, g_rpc_auth_ctx.user_id, vu_user, vu_role))
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Auth service rejected");
    return std::nullopt;
}

// ======== Event publishing ========

// PublishEvent 鈥?publish to RabbitMQ + insert outbox in one call.
inline void PublishEvent(IRabbitPublisher *rabbit, IDatabase *db, int64_t user_id,
                         const char *routing_key, nlohmann::json ev) {
    ev["user_id"] = user_id;
    std::string body = ev.dump();
    if (rabbit)
        rabbit->Publish("rpc.events", routing_key, body);
    if (db)
        db->InsertOutbox(user_id, routing_key, body);
}

// ======== Scope timer ========

// ======== UsernameFromMeta ========

// Extract username carried in gRPC metadata (set by gateway for logging).
inline std::string UsernameFromMeta(grpc::ServerContext *ctx) {
    auto it = ctx->client_metadata().find("username");
    if (it != ctx->client_metadata().end())
        return std::string(it->second.data(), it->second.length());
    return "";
}

// ======== Scope timer ========

class ScopeTimer {
  public:
    using Clock = std::chrono::high_resolution_clock;

    ScopeTimer() : start_(Clock::now()) {}

    int64_t elapsedUs() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_).count();
    }

  private:
    Clock::time_point start_;
};
