// ============================================================
// 协议层: HTTP/1.1 → gRPC/HTTP/2
// 浏览器 ← nginx(epoll+keepalive管道) → 网关(直调gRPC) ← gRPC/HTTP/2 → 各 Service
// nginx upstream keepalive 连接池限死并发面，Gateway 无需线程池
// gRPC 内置 round_robin + DNS aliases 实现负载均衡
// ============================================================
#pragma once
#include <string>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "server/include/redis_client.h"
#include "server/include/tx_manager.h"
#include "server/include/database.h"
#include "circuit_breaker.h"
#include "coro_grpc.h"
#include "coro_task.h"
#include "coro_sched.h"
#include "generated/rpc_auth.grpc.pb.h"
#include "generated/rpc_auth.pb.h"
#include "generated/rpc_spreadsheet.grpc.pb.h"
#include "generated/rpc_spreadsheet.pb.h"
#include "generated/rpc_file.grpc.pb.h"
#include "generated/rpc_file.pb.h"
#include "generated/rpc_health.grpc.pb.h"
#include "generated/rpc_health.pb.h"

// Handler 返回状态 — 区分业务失败和传输故障，防止业务错误误触发熔断
enum class RpcResult { SUCCESS, BUSINESS_FAILURE, TRANSPORT_FAILURE, AUTH_FAILURE, BAD_REQUEST };

class Gateway {
public:
    Gateway(const std::string& listen_addr, int listen_port,
            const std::string& grpc_auth_addr,
            const std::string& grpc_sheet_addr,
            const std::string& grpc_file_addr,
            const std::string& jwt_secret,
            const std::string& web_dir,
            const std::string& redis_host = "",
            const std::string& redis_slave_host = "",
            const std::string& redis_password = "",
            int redis_port = 6379,
            const std::string& redis_sentinel_host = "",
            int redis_sentinel_port = 26379,
            const std::string& redis_master_name = "redis-master");

    bool Start();
    void Stop();

private:
    std::string listen_addr_;
    int listen_port_;
    std::string grpc_auth_addr_, grpc_sheet_addr_, grpc_file_addr_;
    std::string jwt_secret_;
    std::string web_dir_;
    std::string redis_host_, redis_slave_host_, redis_password_;
    int redis_port_;
    std::string redis_sentinel_host_;
    int redis_sentinel_port_;
    std::string redis_master_name_;
    std::unique_ptr<RedisClient> redis_;
    std::unique_ptr<Database> tx_db_;
    std::unique_ptr<TxManager> tx_manager_;

    CircuitBreaker cb_auth_{"auth", 5, 10};
    CircuitBreaker cb_sheet_{"sheet", 5, 30};
    CircuitBreaker cb_file_{"file", 5, 30};
    PerReplicaTracker rep_sheet_{"sheet", 5, 30};  // 副本级追踪
    PerReplicaTracker rep_file_{"file", 5, 30};

    // CQ 协程调度 — 每后端一个 CompletionQueue 事件循环
    CqLoop auth_cq_, sheet_cq_, file_cq_;

    // gRPC Channel + Stub（Channel 含 Subchannel，gRPC 内置 round_robin 轮询）
    std::shared_ptr<grpc::Channel> auth_ch_;
    std::shared_ptr<grpc::Channel> sheet_ch_;
    std::shared_ptr<grpc::Channel> file_ch_;
    std::shared_ptr<grpc::Channel> health_ch_;
    std::unique_ptr<rpc::AuthService::Stub> auth_stub_;
    std::unique_ptr<rpc::SpreadsheetService::Stub> sheet_stub_;
    std::unique_ptr<rpc::FileService::Stub> file_stub_;
    std::unique_ptr<rpc::HealthMonitor::Stub> health_stub_;

    // Route handlers — 返回三态供熔断器区分业务/传输错误
    // username 通过 gRPC metadata 传递（供日志使用），user_id 写入 proto 字段（供 DB 查询使用）。
    // ---- 同步 handler (httplib 8081 使用) ----
    // token_out receives the raw JWT; callers set it as HttpOnly Set-Cookie.
    // Response body contains {"success":true/false} without the token.
    RpcResult HandleLogin(const std::string& body, std::string& response, std::string& token_out);
    RpcResult HandleRegister(const std::string& body, std::string& response, std::string& token_out);
    RpcResult HandleServices(std::string& response);
    RpcResult HandleSheetCreate(const std::string& username, int64_t user_id,
                                const std::string& body, const std::string& idempotency_key,
                                const std::string& raw_token, PerReplicaTracker& rep,
                                std::string& response);
    RpcResult HandleSheetGet(const std::string& username, int64_t user_id,
                             const std::string& body, const std::string& raw_token,
                             PerReplicaTracker& rep, std::string& response);
    RpcResult HandleSheetList(const std::string& username, int64_t user_id,
                              int page, int page_size, const std::string& raw_token,
                              PerReplicaTracker& rep, std::string& response);
    RpcResult HandleSheetUpdate(const std::string& username, int64_t user_id,
                                const std::string& body, const std::string& raw_token,
                                PerReplicaTracker& rep, std::string& response);
    RpcResult HandleSheetDelete(const std::string& username, int64_t user_id,
                                const std::string& body, const std::string& raw_token,
                                PerReplicaTracker& rep, std::string& response);
    RpcResult HandleFileList(const std::string& username, int64_t user_id,
                             int page, int page_size, const std::string& raw_token,
                             PerReplicaTracker& rep, std::string& response);
    RpcResult HandleFileDelete(const std::string& username, int64_t user_id,
                               const std::string& body, const std::string& raw_token,
                               PerReplicaTracker& rep, std::string& response);
    RpcResult HandleTxBegin(const std::string& username, const std::string& body, std::string& response);
    RpcResult HandleHealth(std::string& response);
    RpcResult HandleHistory(std::string& response);
    bool HandleSystemStatus(std::string& response);

    // ---- 协程 handler (h2c 8080 使用, co_await gRPC) ----
    Task<void> HandleLoginCoro(std::string body, std::string& response);
    Task<void> HandleSheetCreateCoro(std::string username, int64_t user_id, std::string body,
                                     std::string idempotency_key, std::string& response);
    Task<void> HandleSheetGetCoro(std::string username, int64_t user_id,
                                  std::string body, std::string& response);
    Task<void> HandleSheetListCoro(std::string username, int64_t user_id,
                                   int page, int page_size, std::string& response);

    // Parses rpc_token from the Cookie header value; old tokens without "uid" get user_id=0.
    bool VerifyAuth(const std::string& cookie_header, std::string& username, int64_t& user_id, std::string& raw_token) const;
    std::string CreateJWT(const std::string& username) const;
    bool WaitForBackends(int timeout_sec = 30);

    // 统一 gRPC 调用上下文：注入 metadata + 设置 deadline
    struct CallOpts {
        std::string username;
        int deadline_sec = 5;
    };
    std::unique_ptr<grpc::ClientContext> MakeCtx(const CallOpts& opts) {
        auto ctx = std::make_unique<grpc::ClientContext>();
        if (!opts.username.empty()) ctx->AddMetadata("username", opts.username);
        ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(opts.deadline_sec));
        return ctx;
    }

    static std::string JsonStr(const std::string& s);
    static std::string JsonGet(const std::string& json, const std::string& key);
    static double   JsonGetNum(const std::string& json, const std::string& key);
};
