#pragma once
#include <grpcpp/grpcpp.h>

#include "generated/rpc_workspace.grpc.pb.h"
#include "generated/rpc_workspace.pb.h"

class ShardedDatabase;

class WorkspaceServiceImpl final : public rpc::WorkspaceService::Service {
   public:
    explicit WorkspaceServiceImpl(ShardedDatabase *db) : db_(db) {}

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
    ShardedDatabase *db_ = nullptr;
    bool isOwner(ShardedDatabase *db, int64_t workspace_id, int64_t owner_id);
    bool isMember(ShardedDatabase *db, int64_t workspace_id, int64_t user_id, std::string &out_role);
};
