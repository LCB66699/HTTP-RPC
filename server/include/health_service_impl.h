// ============================================================
// HealthMonitor — 集群健康监控服务
// 每个节点定时上报心跳，TM/Gateway 查询集群在线状态
// ============================================================
#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <grpcpp/grpcpp.h>
#include "generated/rpc_health.grpc.pb.h"
#include "generated/rpc_health.pb.h"

class ShardedDatabase;

class HealthMonitorImpl final : public rpc::HealthMonitor::Service {
public:
    void SetDatabase(ShardedDatabase* db) { db_ = db; }
    void SetNodeInfo(const std::string& node_id, const std::string& service,
                     const std::string& host, int port);

    grpc::Status Report(grpc::ServerContext* ctx,
                        const rpc::ReportRequest* req,
                        rpc::ReportResponse* resp) override;
    grpc::Status Query(grpc::ServerContext* ctx,
                       const rpc::QueryRequest* req,
                       rpc::QueryResponse* resp) override;

    // 启动心跳线程
    void StartHeartbeat();
    void StopHeartbeat();

private:
    ShardedDatabase* db_ = nullptr;
    std::string node_id_, service_, host_;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;

    void HeartbeatLoop();
};
