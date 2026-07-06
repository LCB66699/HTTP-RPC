#include "auth/workspace_service_impl.h"

#include "shared/base/error_codes.h"
#include "shared/client/database.h"

bool WorkspaceServiceImpl::isOwner(ShardedDatabase *db, int64_t workspace_id, int64_t user_id) {
    return db && db->IsWorkspaceOwner(workspace_id, user_id);
}

bool WorkspaceServiceImpl::isMember(ShardedDatabase *db, int64_t workspace_id, int64_t user_id, std::string &out_role) {
    return db && db->GetWorkspaceMemberRole(workspace_id, user_id, out_role);
}

grpc::Status WorkspaceServiceImpl::Create(grpc::ServerContext *, const rpc::CreateWorkspaceRequest *req,
                                           rpc::WorkspaceResponse *resp) {
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    db_->EnsureWorkspaceTables();

    int64_t id = 0;
    bool ok = db_->CreateWorkspace(req->owner_id(), req->name(), id);
    if (ok && id > 0) {
        db_->AddWorkspaceMember(id, req->owner_id(), req->name(), "admin");
    }
    resp->set_success(ok);
    if (ok) {
        resp->mutable_workspace()->set_id(id);
        resp->mutable_workspace()->set_name(req->name());
        resp->mutable_workspace()->set_owner_id(req->owner_id());
    } else {
        resp->set_error("Failed to create workspace");
        resp->set_error_code(rpc_error::INTERNAL);
    }
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::Get(grpc::ServerContext *, const rpc::GetWorkspaceRequest *req,
                                        rpc::WorkspaceResponse *resp) {
    if (!db_) {
        resp->set_success(false); resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }

    std::string name;
    int64_t owner_id = 0;
    std::string created_at;
    if (!db_->GetWorkspace(req->id(), name, owner_id, created_at)) {
        resp->set_success(false); resp->set_error("Not found");
        resp->set_error_code(rpc_error::NOT_FOUND); return grpc::Status::OK;
    }

    auto *w = resp->mutable_workspace();
    w->set_id(req->id()); w->set_name(name); w->set_owner_id(owner_id);
    w->set_created_at(created_at);
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::List(grpc::ServerContext *, const rpc::ListWorkspacesRequest *req,
                                         rpc::ListWorkspacesResponse *resp) {
    if (!db_) {
        resp->set_success(false); resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }

    std::string json;
    if (!db_->ListWorkspaces(req->user_id(), json)) {
        resp->set_success(false); resp->set_error("Failed to list workspaces");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }

    resp->set_success(true);
    // json is unused here — caller gets workspace list from the repeated field
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::Update(grpc::ServerContext *, const rpc::UpdateWorkspaceRequest *req,
                                           rpc::WorkspaceResponse *resp) {
    if (!db_ || !db_->UpdateWorkspace(req->id(), req->name())) {
        resp->set_success(false); resp->set_error("Update failed");
        resp->set_error_code(rpc_error::NOT_FOUND); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::Delete(grpc::ServerContext *, const rpc::DeleteWorkspaceRequest *req,
                                           rpc::DeleteWorkspaceResponse *resp) {
    if (!db_ || !db_->DeleteWorkspace(req->id())) {
        resp->set_success(false); resp->set_error("Delete failed");
        resp->set_error_code(rpc_error::NOT_FOUND); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::AddMember(grpc::ServerContext *, const rpc::AddMemberRequest *req,
                                              rpc::WorkspaceResponse *resp) {
    if (!db_) {
        resp->set_success(false); resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }

    if (!db_->AddWorkspaceMember(req->id(), req->user_id(), req->username(), req->role())) {
        resp->set_success(false); resp->set_error("Failed to add member");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::RemoveMember(grpc::ServerContext *, const rpc::RemoveMemberRequest *req,
                                                 rpc::WorkspaceResponse *resp) {
    if (!db_) {
        resp->set_success(false); resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }

    if (!db_->RemoveWorkspaceMember(req->id(), req->user_id())) {
        resp->set_success(false); resp->set_error("Failed to remove member");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}
