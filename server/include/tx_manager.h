// ============================================================
// TxManager — 事务管理器 (2PC Coordinator)
// 集成在 Gateway 中：接收 Begin → Prepare 所有 RM → Commit/Rollback
// 后台线程处理超时恢复
// ============================================================
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <grpcpp/grpcpp.h>
#include "generated/rpc_tx.grpc.pb.h"
#include "generated/rpc_tx.pb.h"

class Database;

class TxManager {
public:
    TxManager(Database* tx_db);
    ~TxManager();

    // 注册 RM 实例（service name → gRPC addr）
    void RegisterRM(const std::string& service, std::unique_ptr<rpc::TxResource::Stub> stub);

    // 发起分布式事务
    // xid: 事务 ID（外部生成）
    // participants: [{service, operation}, ...]
    // timeout_seconds: 超时
    // error: 失败原因
    bool Begin(const std::string& xid,
               const std::vector<rpc::TxOperation>& operations,
               int timeout_seconds,
               std::string& error);

    // 启动超时恢复线程
    void StartRecovery();
    void Stop();

private:
    Database* db_;
    std::map<std::string, std::unique_ptr<rpc::TxResource::Stub>> rms_;
    std::mutex mtx_;
    std::atomic<bool> running_{false};
    std::thread recovery_thread_;

    // 对单个 RM 调 Prepare
    bool CallPrepare(const std::string& service, const std::string& xid,
                     const rpc::TxOperation& op, std::string& error);
    // 对单个 RM 调 Commit
    void CallCommit(const std::string& service, const std::string& xid);
    // 对单个 RM 调 Rollback
    void CallRollback(const std::string& service, const std::string& xid);

    // 事务日志
    bool LogTxBegin(const std::string& xid, const std::string& operations_json, int timeout_s);
    bool LogTxStatus(const std::string& xid, const std::string& status);
    void RecoveryLoop();
};
