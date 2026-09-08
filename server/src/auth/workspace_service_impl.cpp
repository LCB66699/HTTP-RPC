#include "auth/workspace_service_impl.h"

#include <nlohmann/json.hpp>

#include <utility>

#include "shared/base/error_codes.h"
#include "shared/base/rpc_interceptor.h"
#include "shared/client/database.h"

namespace {

class ShardedWorkspaceStore final : public WorkspaceStore {
   public:
    explicit ShardedWorkspaceStore(ShardedDatabase *db) : db_(db) {}

    bool EnsureWorkspaceTables() override { return db_ && db_->EnsureWorkspaceTables(); }
    bool CreateWorkspace(int64_t owner_id, const std::string &name, int64_t &out_id) override {
        return db_ && db_->CreateWorkspace(owner_id, name, out_id);
    }
    bool GetWorkspace(int64_t id, std::string &name, int64_t &owner_id, std::string &created_at) override {
        return db_ && db_->GetWorkspace(id, name, owner_id, created_at);
    }
    bool ListWorkspaces(int64_t user_id, std::string &out_json) override {
        return db_ && db_->ListWorkspaces(user_id, out_json);
    }
    bool ListWorkspaceMembers(int64_t workspace_id, std::string &out_json) override {
        return db_ && db_->ListWorkspaceMembers(workspace_id, out_json);
    }
    bool UpdateWorkspace(int64_t id, const std::string &name) override {
        return db_ && db_->UpdateWorkspace(id, name);
    }
    bool DeleteWorkspace(int64_t id) override { return db_ && db_->DeleteWorkspace(id); }
    bool AddWorkspaceMember(int64_t workspace_id, int64_t user_id, const std::string &username,
                            const std::string &role) override {
        return db_ && db_->AddWorkspaceMember(workspace_id, user_id, username, role);
    }
    bool RemoveWorkspaceMember(int64_t workspace_id, int64_t user_id) override {
        return db_ && db_->RemoveWorkspaceMember(workspace_id, user_id);
    }
    bool IsWorkspaceOwner(int64_t workspace_id, int64_t user_id) override {
        return db_ && db_->IsWorkspaceOwner(workspace_id, user_id);
    }
    bool GetWorkspaceMemberRole(int64_t workspace_id, int64_t user_id, std::string &out_role) override {
        return db_ && db_->GetWorkspaceMemberRole(workspace_id, user_id, out_role);
    }
    int64_t GetUserId(const std::string &username) override { return db_ ? db_->GetUserId(username) : -1; }

   private:
    ShardedDatabase *db_;
};

template <typename Response>
grpc::Status Fail(Response *resp, const char *message, int error_code) {
    resp->set_success(false);
    resp->set_error(message);
    resp->set_error_code(error_code);
    return grpc::Status::OK;
}

bool HasPrincipal() {
    return g_rpc_auth_ctx.authenticated && g_rpc_auth_ctx.user_id > 0;
}

bool IsValidMemberRole(const std::string &role) {
    return role == "admin" || role == "editor" || role == "viewer";
}

}  // namespace

WorkspaceServiceImpl::WorkspaceServiceImpl(ShardedDatabase *db)
    : owned_store_(std::make_unique<ShardedWorkspaceStore>(db)), store_(owned_store_.get()) {}

bool WorkspaceServiceImpl::canRead(int64_t workspace_id, int64_t user_id) {
    if (!store_) return false;
    if (store_->IsWorkspaceOwner(workspace_id, user_id)) return true;
    std::string role;
    return store_->GetWorkspaceMemberRole(workspace_id, user_id, role);
}

bool WorkspaceServiceImpl::canManage(int64_t workspace_id, int64_t user_id) {
    if (!store_) return false;
    if (store_->IsWorkspaceOwner(workspace_id, user_id)) return true;
    std::string role;
    return store_->GetWorkspaceMemberRole(workspace_id, user_id, role) && role == "admin";
}

grpc::Status WorkspaceServiceImpl::Create(grpc::ServerContext *, const rpc::CreateWorkspaceRequest *req,
                                           rpc::WorkspaceResponse *resp) {
    if (!HasPrincipal()) return Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
    if (!store_) return Fail(resp, "Database not available", rpc_error::INTERNAL);
    if (req->name().empty()) return Fail(resp, "Workspace name required", rpc_error::BAD_REQUEST);

    if (!store_->EnsureWorkspaceTables()) return Fail(resp, "Database not available", rpc_error::INTERNAL);

    int64_t id = 0;
    const int64_t principal_id = g_rpc_auth_ctx.user_id;
    bool ok = store_->CreateWorkspace(principal_id, req->name(), id);
    if (ok && id > 0 && !store_->AddWorkspaceMember(id, principal_id, g_rpc_auth_ctx.username, "admin")) {
        store_->DeleteWorkspace(id);
        ok = false;
    }
    resp->set_success(ok);
    if (ok) {
        resp->mutable_workspace()->set_id(id);
        resp->mutable_workspace()->set_name(req->name());
        resp->mutable_workspace()->set_owner_id(principal_id);
    } else {
        resp->set_error("Failed to create workspace");
        resp->set_error_code(rpc_error::INTERNAL);
    }
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::Get(grpc::ServerContext *, const rpc::GetWorkspaceRequest *req,
                                        rpc::WorkspaceResponse *resp) {
    if (!HasPrincipal()) return Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
    if (!store_) return Fail(resp, "Database not available", rpc_error::INTERNAL);
    if (!canRead(req->id(), g_rpc_auth_ctx.user_id)) return Fail(resp, "Access denied", rpc_error::FORBIDDEN);

    std::string name;
    int64_t owner_id = 0;
    std::string created_at;
    if (!store_->GetWorkspace(req->id(), name, owner_id, created_at)) {
        resp->set_success(false); resp->set_error("Not found");
        resp->set_error_code(rpc_error::NOT_FOUND); return grpc::Status::OK;
    }

    auto *w = resp->mutable_workspace();
    w->set_id(req->id()); w->set_name(name); w->set_owner_id(owner_id);
    w->set_created_at(created_at);
    std::string members_json;
    if (!store_->ListWorkspaceMembers(req->id(), members_json)) {
        return Fail(resp, "Failed to list workspace members", rpc_error::INTERNAL);
    }
    try {
        const auto members = nlohmann::json::parse(members_json);
        if (!members.is_array()) return Fail(resp, "Invalid workspace member data", rpc_error::INTERNAL);
        for (const auto &member : members) {
            if (!member.is_object()) return Fail(resp, "Invalid workspace member data", rpc_error::INTERNAL);
            auto *item = resp->add_members();
            item->set_user_id(member.value("user_id", int64_t{0}));
            item->set_username(member.value("username", std::string{}));
            item->set_role(member.value("role", std::string{}));
            item->set_joined_at(member.value("joined_at", std::string{}));
        }
    } catch (const nlohmann::json::exception &) {
        return Fail(resp, "Invalid workspace member data", rpc_error::INTERNAL);
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::List(grpc::ServerContext *, const rpc::ListWorkspacesRequest *,
                                         rpc::ListWorkspacesResponse *resp) {
    if (!HasPrincipal()) return Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
    if (!store_) return Fail(resp, "Database not available", rpc_error::INTERNAL);

    std::string json;
    if (!store_->ListWorkspaces(g_rpc_auth_ctx.user_id, json)) {
        resp->set_success(false); resp->set_error("Failed to list workspaces");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }

    try {
        const auto workspaces = nlohmann::json::parse(json);
        if (!workspaces.is_array()) return Fail(resp, "Invalid workspace data", rpc_error::INTERNAL);
        for (const auto &workspace : workspaces) {
            if (!workspace.is_object()) return Fail(resp, "Invalid workspace data", rpc_error::INTERNAL);
            auto *item = resp->add_workspaces();
            item->set_id(workspace.value("id", int64_t{0}));
            item->set_name(workspace.value("name", std::string{}));
            item->set_owner_id(workspace.value("owner_id", int64_t{0}));
            item->set_created_at(workspace.value("created_at", std::string{}));
        }
    } catch (const nlohmann::json::exception &) {
        return Fail(resp, "Invalid workspace data", rpc_error::INTERNAL);
    }
    resp->set_success(true);
    // json is unused here — caller gets workspace list from the repeated field
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::Update(grpc::ServerContext *, const rpc::UpdateWorkspaceRequest *req,
                                           rpc::WorkspaceResponse *resp) {
    if (!HasPrincipal()) return Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
    if (!store_) return Fail(resp, "Database not available", rpc_error::INTERNAL);
    if (!canManage(req->id(), g_rpc_auth_ctx.user_id)) return Fail(resp, "Access denied", rpc_error::FORBIDDEN);
    if (req->name().empty()) return Fail(resp, "Workspace name required", rpc_error::BAD_REQUEST);
    if (!store_->UpdateWorkspace(req->id(), req->name())) {
        resp->set_success(false); resp->set_error("Update failed");
        resp->set_error_code(rpc_error::NOT_FOUND); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::Delete(grpc::ServerContext *, const rpc::DeleteWorkspaceRequest *req,
                                           rpc::DeleteWorkspaceResponse *resp) {
    if (!HasPrincipal()) return Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
    if (!store_) return Fail(resp, "Database not available", rpc_error::INTERNAL);
    if (!canManage(req->id(), g_rpc_auth_ctx.user_id)) return Fail(resp, "Access denied", rpc_error::FORBIDDEN);
    if (!store_->DeleteWorkspace(req->id())) {
        resp->set_success(false); resp->set_error("Delete failed");
        resp->set_error_code(rpc_error::NOT_FOUND); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::AddMember(grpc::ServerContext *, const rpc::AddMemberRequest *req,
                                              rpc::WorkspaceResponse *resp) {
    if (!HasPrincipal()) return Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
    if (!store_) return Fail(resp, "Database not available", rpc_error::INTERNAL);
    if (!canManage(req->id(), g_rpc_auth_ctx.user_id)) return Fail(resp, "Access denied", rpc_error::FORBIDDEN);
    if (req->username().empty()) return Fail(resp, "Username required", rpc_error::BAD_REQUEST);
    if (!IsValidMemberRole(req->role())) return Fail(resp, "Invalid member role", rpc_error::BAD_REQUEST);

    const int64_t target_user_id = store_->GetUserId(req->username());
    if (target_user_id <= 0) return Fail(resp, "User not found", rpc_error::NOT_FOUND);
    if (req->user_id() > 0 && req->user_id() != target_user_id) {
        return Fail(resp, "User identity mismatch", rpc_error::BAD_REQUEST);
    }

    if (!store_->AddWorkspaceMember(req->id(), target_user_id, req->username(), req->role())) {
        resp->set_success(false); resp->set_error("Failed to add member");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status WorkspaceServiceImpl::RemoveMember(grpc::ServerContext *, const rpc::RemoveMemberRequest *req,
                                                 rpc::WorkspaceResponse *resp) {
    if (!HasPrincipal()) return Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
    if (!store_) return Fail(resp, "Database not available", rpc_error::INTERNAL);
    if (!canManage(req->id(), g_rpc_auth_ctx.user_id)) return Fail(resp, "Access denied", rpc_error::FORBIDDEN);
    if (req->user_id() <= 0) return Fail(resp, "Invalid user", rpc_error::BAD_REQUEST);
    if (store_->IsWorkspaceOwner(req->id(), req->user_id())) {
        return Fail(resp, "Workspace owner cannot be removed", rpc_error::BAD_REQUEST);
    }

    if (!store_->RemoveWorkspaceMember(req->id(), req->user_id())) {
        resp->set_success(false); resp->set_error("Failed to remove member");
        resp->set_error_code(rpc_error::INTERNAL); return grpc::Status::OK;
    }
    resp->set_success(true);
    return grpc::Status::OK;
}
