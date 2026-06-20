#include "shared/health_service_impl.h"

#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <sstream>

#include "shared/redis_client.h"

void HealthMonitorImpl::SetNodeInfo(const std::string &node_id, const std::string &service, const std::string &host,
                                    int port) {
    node_id_ = node_id;
    service_ = service;
    host_ = host;
    port_ = port;
}

grpc::Status HealthMonitorImpl::Report(grpc::ServerContext *, const rpc::ReportRequest *req,
                                       rpc::ReportResponse *resp) {
    if (!redis_ || !redis_->IsConnected()) {
        resp->set_success(false);
        return grpc::Status::OK;
    }
    nlohmann::json hb;
    hb["node_id"] = req->node_id();
    hb["service"] = req->service();
    hb["host"] = req->host();
    hb["port"] = req->port();
    hb["version"] = req->version();
    hb["status"] = "ONLINE";
    resp->set_success(redis_->HSetJSON("heartbeats", req->node_id(), hb.dump(), 30));
    return grpc::Status::OK;
}

grpc::Status HealthMonitorImpl::Query(grpc::ServerContext *, const rpc::QueryRequest *, rpc::QueryResponse *resp) {
    // Query 已移�?Gateway �?�?�?Redis KEYS hb:* 聚合
    // 保留 gRPC 接口兼容，返回空成功
    resp->set_total_online(0);
    resp->set_total_offline(0);
    return grpc::Status::OK;
}

void HealthMonitorImpl::StartHeartbeat() {
    if (!redis_)
        return;
    running_ = true;
    heartbeat_thread_ = std::thread(&HealthMonitorImpl::HeartbeatLoop, this);
    printf("[Health] Heartbeat started (Redis, every 10s)\n");
}

void HealthMonitorImpl::StopHeartbeat() {
    running_ = false;
    if (heartbeat_thread_.joinable())
        heartbeat_thread_.join();
}

void HealthMonitorImpl::HeartbeatLoop() {
    while (running_) {
        if (redis_ && redis_->IsConnected()) {
            nlohmann::json hb;
            hb["node_id"] = node_id_;
            hb["service"] = service_;
            hb["host"] = host_;
            hb["port"] = port_;
            hb["version"] = "1.0";
            hb["status"] = "ONLINE";
            redis_->HSetJSON("heartbeats", node_id_, hb.dump(), 30);
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}
