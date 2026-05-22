#include "tx_manager.h"
#include "database.h"
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <nlohmann/json.hpp>

TxManager::TxManager(Database* tx_db) : db_(tx_db) {
    // 确保 tx_log 表存在
    if (db_) {
        db_->Exec("CREATE TABLE IF NOT EXISTS tx_log ("
                  "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
                  "xid VARCHAR(64) UNIQUE NOT NULL, "
                  "status VARCHAR(16) NOT NULL, "
                  "participants_json JSON, "
                  "created_at DATETIME DEFAULT NOW(), "
                  "updated_at DATETIME DEFAULT NOW(), "
                  "timeout_at DATETIME NOT NULL)");
    }
}

TxManager::~TxManager() { Stop(); }

void TxManager::RegisterRM(const std::string& service,
                           std::unique_ptr<rpc::TxResource::Stub> stub) {
    std::lock_guard<std::mutex> lock(mtx_);
    rms_[service] = std::move(stub);
}

bool TxManager::Begin(const std::string& xid,
                      const std::vector<rpc::TxOperation>& operations,
                      int timeout_seconds,
                      std::string& error) {
    if (operations.empty()) {
        error = "No participants";
        return false;
    }

    nlohmann::json ops_json = nlohmann::json::array();
    for (const auto& op : operations) {
        ops_json.push_back({{"service", op.service()}, {"type", op.type()}});
    }

    LogTxBegin(xid, ops_json.dump(), timeout_seconds);

    // ---- Phase 1: PREPARE ----
    printf("[TM] %s Phase 1 PREPARE (%zu participants)\n", xid.c_str(), operations.size());
    bool all_ready = true;
    std::vector<std::string> prepared_services;

    for (const auto& op : operations) {
        std::string prepare_error;
        bool ready = CallPrepare(op.service(), xid, op, prepare_error);
        if (!ready) {
            all_ready = false;
            error = op.service() + " prepare failed: " + prepare_error;
            printf("[TM] %s PREPARE FAILED: %s\n", xid.c_str(), error.c_str());
            break;
        }
        prepared_services.push_back(op.service());
    }

    // ---- Phase 2 ----
    if (all_ready) {
        printf("[TM] %s Phase 2 COMMIT\n", xid.c_str());
        LogTxStatus(xid, "COMMITTING");
        for (const auto& svc : prepared_services) {
            CallCommit(svc, xid);
        }
        LogTxStatus(xid, "COMMITTED");
        printf("[TM] %s COMMITTED\n", xid.c_str());
        return true;
    } else {
        printf("[TM] %s Phase 2 ROLLBACK\n", xid.c_str());
        LogTxStatus(xid, "ROLLING_BACK");
        for (const auto& svc : prepared_services) {
            CallRollback(svc, xid);
        }
        LogTxStatus(xid, "ROLLED_BACK");
        printf("[TM] %s ROLLED_BACK\n", xid.c_str());
        return false;
    }
}

bool TxManager::CallPrepare(const std::string& service, const std::string& xid,
                            const rpc::TxOperation& op, std::string& error) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = rms_.find(service);
    if (it == rms_.end()) {
        error = "Unknown service: " + service;
        return false;
    }

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    rpc::PrepareRequest req;
    rpc::PrepareResponse resp;
    req.set_xid(xid);
    *req.mutable_operation() = op;

    auto st = it->second->Prepare(&ctx, req, &resp);
    if (!st.ok()) {
        error = "gRPC error: " + st.error_message();
        return false;
    }
    if (!resp.ready()) {
        error = resp.error();
        return false;
    }
    return true;
}

void TxManager::CallCommit(const std::string& service, const std::string& xid) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = rms_.find(service);
    if (it == rms_.end()) return;

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    rpc::CommitRequest req;
    rpc::CommitResponse resp;
    req.set_xid(xid);
    it->second->Commit(&ctx, req, &resp);
}

void TxManager::CallRollback(const std::string& service, const std::string& xid) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = rms_.find(service);
    if (it == rms_.end()) return;

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    rpc::RollbackRequest req;
    rpc::RollbackResponse resp;
    req.set_xid(xid);
    it->second->Rollback(&ctx, req, &resp);
}

bool TxManager::LogTxBegin(const std::string& xid, const std::string& ops_json, int timeout_s) {
    if (!db_) return false;
    std::string sql = "INSERT INTO tx_log (xid, status, participants_json, timeout_at) VALUES ('"
        + xid + "', 'BEGIN', '" + ops_json + "', NOW() + INTERVAL " + std::to_string(timeout_s) + " SECOND)";
    return db_->Exec(sql);
}

bool TxManager::LogTxStatus(const std::string& xid, const std::string& status) {
    if (!db_) return false;
    std::string sql = "UPDATE tx_log SET status='" + status
        + "', updated_at=NOW() WHERE xid='" + xid + "'";
    return db_->Exec(sql);
}

void TxManager::StartRecovery() {
    running_ = true;
    recovery_thread_ = std::thread(&TxManager::RecoveryLoop, this);
    printf("[TM] Recovery thread started\n");
}

void TxManager::Stop() {
    running_ = false;
    if (recovery_thread_.joinable()) recovery_thread_.join();
}

void TxManager::RecoveryLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!db_) continue;

        // 查找超时未完成的事务
        std::string sql = "SELECT xid, status FROM tx_log "
                          "WHERE status IN ('BEGIN','PREPARED','COMMITTING') "
                          "AND timeout_at < NOW()";
        // 使用数据库查询
        // 简化处理：标记为 ROLLING_BACK 并回滚
        // 对于 COMMITTING 的事务，重试 COMMIT（幂等）

        printf("[TM] Recovery scan (skipped, no active timeout handling for now)\n");
    }
}
