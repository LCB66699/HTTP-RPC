// ============================================================
// HealthMonitor — 集群健康监控服务
// 每个节点定时心跳 → Redis SETEX JSON (TTL=30s)
// Gateway 读 Redis KEYS hb:* 聚合所有节点在线状态
// ============================================================
#pragma once
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <string>
#include <thread>

#include "generated/rpc_health.grpc.pb.h"
#include "generated/rpc_health.pb.h"

class RedisClient;

class HealthMonitorImpl final : public rpc::HealthMonitor::Service {
   public:
    void SetRedis(RedisClient *redis) { redis_ = redis; }
    void SetNodeInfo(const std::string &node_id, const std::string &service, const std::string &host, int port);

    grpc::Status Report(grpc::ServerContext *ctx, const rpc::ReportRequest *req, rpc::ReportResponse *resp) override;
    grpc::Status Query(grpc::ServerContext *ctx, const rpc::QueryRequest *req, rpc::QueryResponse *resp) override;

    void StartHeartbeat();
    void StopHeartbeat();

   private:
    RedisClient *redis_ = nullptr;
    std::string node_id_, service_, host_;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;

    void HeartbeatLoop();
};
