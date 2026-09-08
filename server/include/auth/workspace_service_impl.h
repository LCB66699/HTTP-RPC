#pragma once
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

#include "generated/rpc_workspace.grpc.pb.h"
#include "generated/rpc_workspace.pb.h"

class ShardedDatabase;

class WorkspaceStore {
   public:
    virtual ~WorkspaceStore() = default;
    virtual bool EnsureWorkspaceTables() = 0;
    virtual bool CreateWorkspace(int64_t owner_id, const std::string &name, int64_t &out_id) = 0;
    virtual bool GetWorkspace(int64_t id, std::string &name, int64_t &owner_id, std::string &created_at) = 0;
    virtual bool ListWorkspaces(int64_t user_id, std::string &out_json) = 0;
    virtual bool ListWorkspaceMembers(int64_t workspace_id, std::string &out_json) = 0;
    virtual bool UpdateWorkspace(int64_t id, const std::string &name) = 0;
    virtual bool DeleteWorkspace(int64_t id) = 0;
    virtual bool AddWorkspaceMember(int64_t workspace_id, int64_t user_id, const std::string &username,
                                    const std::string &role) = 0;
    virtual bool RemoveWorkspaceMember(int64_t workspace_id, int64_t user_id) = 0;
    virtual bool IsWorkspaceOwner(int64_t workspace_id, int64_t user_id) = 0;
    virtual bool GetWorkspaceMemberRole(int64_t workspace_id, int64_t user_id, std::string &out_role) = 0;
    virtual int64_t GetUserId(const std::string &username) = 0;
};

class WorkspaceServiceImpl final : public rpc::WorkspaceService::Service {
   public:
    explicit WorkspaceServiceImpl(ShardedDatabase *db);
    explicit WorkspaceServiceImpl(WorkspaceStore *store) : store_(store) {}

    grpc::Status Create(grpc::ServerContext *ctx, const rpc::CreateWorkspaceRequest *req,
                        rpc::WorkspaceResponse *resp) override;
    grpc::Status Get(grpc::ServerContext *ctx, const rpc::GetWorkspaceRequest *req,
                     rpc::WorkspaceResponse *resp) override;
    grpc::Status List(grpc::ServerContext *ctx, const rpc::ListWorkspacesRequest *req,
                      rpc::ListWorkspacesResponse *resp) override;
    grpc::Status Update(grpc::ServerContext *ctx, const rpc::UpdateWorkspaceRequest *req,
                        rpc::WorkspaceResponse *resp) override;
    grpc::Status Delete(grpc::ServerContext *ctx, const rpc::DeleteWorkspaceRequest *req,
                        rpc::DeleteWorkspaceResponse *resp) override;
    grpc::Status AddMember(grpc::ServerContext *ctx, const rpc::AddMemberRequest *req,
                           rpc::WorkspaceResponse *resp) override;
    grpc::Status RemoveMember(grpc::ServerContext *ctx, const rpc::RemoveMemberRequest *req,
                              rpc::WorkspaceResponse *resp) override;

   private:
    std::unique_ptr<WorkspaceStore> owned_store_;
    WorkspaceStore *store_ = nullptr;

    bool canRead(int64_t workspace_id, int64_t user_id);
    bool canManage(int64_t workspace_id, int64_t user_id);
};
