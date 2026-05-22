#include "health_service_impl.h"
#include "database.h"
#include <mysql/mysql.h>
#include <cstdio>
#include <chrono>
#include <sstream>

void HealthMonitorImpl::SetNodeInfo(const std::string& node_id, const std::string& service,
                                     const std::string& host, int port) {
    node_id_ = node_id;
    service_ = service;
    host_ = host;
    port_ = port;
}

grpc::Status HealthMonitorImpl::Report(grpc::ServerContext*,
                                        const rpc::ReportRequest* req,
                                        rpc::ReportResponse* resp) {
    if (!db_) {
        resp->set_success(false);
        return grpc::Status::OK;
    }

    // UPSERT: INSERT or UPDATE heartbeat
    std::ostringstream sql;
    sql << "INSERT INTO health_status (node_id, service, host, port, version, last_heartbeat, status) "
        << "VALUES ('" << req->node_id() << "','" << req->service() << "','"
        << req->host() << "'," << req->port() << ",'" << req->version()
        << "',NOW(),'ONLINE') "
        << "ON DUPLICATE KEY UPDATE version='" << req->version()
        << "', last_heartbeat=NOW(), status='ONLINE', host='" << req->host()
        << "', port=" << req->port();

    resp->set_success(db_->Exec(sql.str()));
    return grpc::Status::OK;
}

grpc::Status HealthMonitorImpl::Query(grpc::ServerContext*,
                                       const rpc::QueryRequest*,
                                       rpc::QueryResponse* resp) {
    if (!db_) return grpc::Status::OK;

    // 标记超时节点为 OFFLINE（30s 无心跳）
    db_->Exec("UPDATE health_status SET status='OFFLINE' "
              "WHERE status='ONLINE' AND last_heartbeat < NOW() - INTERVAL 30 SECOND");

    // 查询所有节点（直接走读库）
    std::string sql = "SELECT node_id, service, host, port, status, version, "
                      "last_heartbeat, TIMESTAMPDIFF(SECOND, last_heartbeat, NOW()) "
                      "FROM health_status ORDER BY service, node_id";
    MYSQL_RES* res = nullptr;
    {
        auto* conn = db_->GetConnection();
        if (!conn) return grpc::Status::OK;
        if (mysql_query(conn, sql.c_str()) != 0) return grpc::Status::OK;
        res = mysql_store_result(conn);
    }
    if (!res) return grpc::Status::OK;

    int online = 0, offline = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        auto* node = resp->add_nodes();
        node->set_node_id(row[0] ? row[0] : "");
        node->set_service(row[1] ? row[1] : "");
        node->set_host(row[2] ? row[2] : "");
        node->set_port(row[3] ? std::stoi(row[3]) : 0);
        node->set_status(row[4] ? row[4] : "UNKNOWN");
        node->set_version(row[5] ? row[5] : "");
        node->set_last_heartbeat(row[6] ? row[6] : "");
        node->set_uptime_seconds(row[7] ? std::stoi(row[7]) : 0);

        if (node->status() == "ONLINE") online++; else offline++;
    }
    mysql_free_result(res);

    resp->set_total_online(online);
    resp->set_total_offline(offline);
    return grpc::Status::OK;
}

void HealthMonitorImpl::StartHeartbeat() {
    if (!db_) return;

    // 确保 health_status 表存在
    db_->Exec("CREATE TABLE IF NOT EXISTS health_status ("
              "node_id VARCHAR(64) PRIMARY KEY, "
              "service VARCHAR(64), "
              "host VARCHAR(64), "
              "port INT DEFAULT 0, "
              "status VARCHAR(16) DEFAULT 'OFFLINE', "
              "version VARCHAR(32) DEFAULT '', "
              "last_heartbeat DATETIME DEFAULT NOW())");

    running_ = true;
    heartbeat_thread_ = std::thread(&HealthMonitorImpl::HeartbeatLoop, this);
}

void HealthMonitorImpl::StopHeartbeat() {
    running_ = false;
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

void HealthMonitorImpl::HeartbeatLoop() {
    // 启动后先等 mysql 就绪
    std::this_thread::sleep_for(std::chrono::seconds(2));

    while (running_) {
        std::ostringstream sql;
        sql << "INSERT INTO health_status (node_id, service, host, port, version, last_heartbeat, status) "
            << "VALUES ('" << node_id_ << "','" << service_ << "','"
            << host_ << "'," << port_ << ",'1.0',NOW(),'ONLINE') "
            << "ON DUPLICATE KEY UPDATE last_heartbeat=NOW(), status='ONLINE', "
            << "host='" << host_ << "', port=" << port_;
        db_->Exec(sql.str());
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}
