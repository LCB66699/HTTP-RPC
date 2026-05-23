// ============================================================
// TxResource — Resource Manager 基类
// 每个 gRPC Service 继承此类，实现 2PC 的 Prepare/Commit/Rollback
// undo_log 用 JSON 快照回滚
// ============================================================
#pragma once
#include <string>
#include <functional>
#include <grpcpp/grpcpp.h>
#include "generated/rpc_tx.grpc.pb.h"
#include "generated/rpc_tx.pb.h"

class ShardedDatabase;

class TxResource : public rpc::TxResource::Service {
public:
    void SetDatabase(ShardedDatabase* db) { db_ = db; }

    // 注册业务操作处理器（public，main 中调用）
    using OpHandler = std::function<bool(const std::string& params_json, std::string& error)>;
    void RegisterHandler(const std::string& type, OpHandler handler);

    // 2PC 协议
    grpc::Status Prepare(grpc::ServerContext* ctx,
                         const rpc::PrepareRequest* req,
                         rpc::PrepareResponse* resp) override;
    grpc::Status Commit(grpc::ServerContext* ctx,
                        const rpc::CommitRequest* req,
                        rpc::CommitResponse* resp) override;
    grpc::Status Rollback(grpc::ServerContext* ctx,
                          const rpc::RollbackRequest* req,
                          rpc::RollbackResponse* resp) override;

protected:
    ShardedDatabase* db_ = nullptr;

    // undo log 操作（子类可调用）
    bool WriteUndoLog(const std::string& xid, const std::string& table_name,
                      int64_t row_id, const std::string& before_snapshot);
    bool GetUndoLog(const std::string& xid, std::string& table_name,
                    int64_t& row_id, std::string& before_snapshot);
    bool ClearUndoLog(const std::string& xid);

private:
    std::map<std::string, OpHandler> handlers_;
    mutable std::mutex mtx_;
};
