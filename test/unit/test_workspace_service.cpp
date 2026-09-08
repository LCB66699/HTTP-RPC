#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "auth/workspace_service_impl.h"
#include "shared/base/error_codes.h"
#include "shared/base/rpc_interceptor.h"

class FakeWorkspaceStore final : public WorkspaceStore {
public:
    int64_t owner_id = 10;
    std::unordered_map<int64_t, std::string> roles;
    std::unordered_map<std::string, int64_t> users;
    int get_calls = 0;
    int update_calls = 0;
    int delete_calls = 0;
    int add_calls = 0;
    int remove_calls = 0;
    int64_t last_user_id = 0;
    int64_t listed_user_id = 0;
    std::string members_json = R"([{"user_id": 10, "username": "owner", "role": "admin", "joined_at": "now"}])";

    bool EnsureWorkspaceTables() override { return true; }
    bool CreateWorkspace(int64_t uid, const std::string &, int64_t &out_id) override {
        last_user_id = uid;
        out_id = 77;
        return true;
    }
    bool GetWorkspace(int64_t, std::string &name, int64_t &owner, std::string &created_at) override {
        ++get_calls;
        name = "workspace";
        owner = owner_id;
        created_at = "now";
        return true;
    }
    bool ListWorkspaces(int64_t uid, std::string &out_json) override {
        listed_user_id = uid;
        out_json = R"([{"id": 88, "name": "visible", "owner_id": 20, "created_at": "now"}])";
        return true;
    }
    bool ListWorkspaceMembers(int64_t, std::string &out_json) override {
        out_json = members_json;
        return true;
    }
    bool UpdateWorkspace(int64_t, const std::string &) override { ++update_calls; return true; }
    bool DeleteWorkspace(int64_t) override { ++delete_calls; return true; }
    bool AddWorkspaceMember(int64_t, int64_t uid, const std::string &, const std::string &) override {
        ++add_calls;
        last_user_id = uid;
        return true;
    }
    bool RemoveWorkspaceMember(int64_t, int64_t uid) override {
        ++remove_calls;
        last_user_id = uid;
        return true;
    }
    bool IsWorkspaceOwner(int64_t, int64_t uid) override { return uid == owner_id; }
    bool GetWorkspaceMemberRole(int64_t, int64_t uid, std::string &role) override {
        auto it = roles.find(uid);
        if (it == roles.end()) return false;
        role = it->second;
        return true;
    }
    int64_t GetUserId(const std::string &username) override {
        auto it = users.find(username);
        return it == users.end() ? -1 : it->second;
    }
};

class WorkspaceAuthGuard {
public:
    WorkspaceAuthGuard(bool authenticated, int64_t uid, std::string username = "caller")
        : saved_(g_rpc_auth_ctx) {
        g_rpc_auth_ctx = {std::move(username), uid, authenticated};
    }
    ~WorkspaceAuthGuard() { g_rpc_auth_ctx = saved_; }
private:
    AuthContext saved_;
};

TEST(WorkspaceServiceAuthorization, GetRejectsNonMemberBeforeReadingWorkspace) {
    FakeWorkspaceStore store;
    WorkspaceServiceImpl service(&store);
    WorkspaceAuthGuard auth(true, 99);
    rpc::GetWorkspaceRequest req;
    rpc::WorkspaceResponse resp;
    req.set_id(7);

    service.Get(nullptr, &req, &resp);

    EXPECT_FALSE(resp.success());
    EXPECT_EQ(resp.error_code(), rpc_error::FORBIDDEN);
    EXPECT_EQ(store.get_calls, 0);
}

TEST(WorkspaceServiceAuthorization, MemberCanReadButCannotUpdate) {
    FakeWorkspaceStore store;
    store.roles[20] = "viewer";
    WorkspaceServiceImpl service(&store);
    WorkspaceAuthGuard auth(true, 20);
    rpc::GetWorkspaceRequest get_req;
    rpc::WorkspaceResponse get_resp;
    get_req.set_id(7);
    service.Get(nullptr, &get_req, &get_resp);

    rpc::UpdateWorkspaceRequest update_req;
    rpc::WorkspaceResponse update_resp;
    update_req.set_id(7);
    update_req.set_name("renamed");
    service.Update(nullptr, &update_req, &update_resp);

    EXPECT_TRUE(get_resp.success());
    ASSERT_EQ(get_resp.members_size(), 1);
    EXPECT_EQ(get_resp.members(0).username(), "owner");
    EXPECT_FALSE(update_resp.success());
    EXPECT_EQ(update_resp.error_code(), rpc_error::FORBIDDEN);
    EXPECT_EQ(store.update_calls, 0);
}

TEST(WorkspaceServiceAuthorization, AdminCanUpdateAndManageMembers) {
    FakeWorkspaceStore store;
    store.roles[20] = "admin";
    store.users["target"] = 42;
    WorkspaceServiceImpl service(&store);
    WorkspaceAuthGuard auth(true, 20);

    rpc::UpdateWorkspaceRequest update_req;
    rpc::WorkspaceResponse update_resp;
    update_req.set_id(7);
    update_req.set_name("renamed");
    service.Update(nullptr, &update_req, &update_resp);

    rpc::AddMemberRequest add_req;
    rpc::WorkspaceResponse add_resp;
    add_req.set_id(7);
    add_req.set_username("target");
    add_req.set_role("viewer");
    service.AddMember(nullptr, &add_req, &add_resp);

    EXPECT_TRUE(update_resp.success());
    EXPECT_TRUE(add_resp.success());
    EXPECT_EQ(store.update_calls, 1);
    EXPECT_EQ(store.add_calls, 1);
    EXPECT_EQ(store.last_user_id, 42);
}

TEST(WorkspaceServiceAuthorization, AddMemberRejectsMismatchedResolvedUserId) {
    FakeWorkspaceStore store;
    store.roles[20] = "admin";
    store.users["target"] = 42;
    WorkspaceServiceImpl service(&store);
    WorkspaceAuthGuard auth(true, 20);
    rpc::AddMemberRequest req;
    rpc::WorkspaceResponse resp;
    req.set_id(7);
    req.set_user_id(999);
    req.set_username("target");
    req.set_role("viewer");

    service.AddMember(nullptr, &req, &resp);

    EXPECT_FALSE(resp.success());
    EXPECT_EQ(resp.error_code(), rpc_error::BAD_REQUEST);
    EXPECT_EQ(store.add_calls, 0);
}

TEST(WorkspaceServiceAuthorization, RequestIdentityCannotOverrideAuthenticatedPrincipal) {
    FakeWorkspaceStore store;
    WorkspaceServiceImpl service(&store);
    WorkspaceAuthGuard auth(true, 20, "actual-owner");

    rpc::CreateWorkspaceRequest create_req;
    rpc::WorkspaceResponse create_resp;
    create_req.set_name("new workspace");
    create_req.set_owner_id(999);
    service.Create(nullptr, &create_req, &create_resp);

    rpc::ListWorkspacesRequest list_req;
    rpc::ListWorkspacesResponse list_resp;
    list_req.set_user_id(999);
    service.List(nullptr, &list_req, &list_resp);

    EXPECT_TRUE(create_resp.success());
    EXPECT_EQ(create_resp.workspace().owner_id(), 20);
    EXPECT_EQ(store.listed_user_id, 20);
    ASSERT_EQ(list_resp.workspaces_size(), 1);
    EXPECT_EQ(list_resp.workspaces(0).id(), 88);
    EXPECT_EQ(list_resp.workspaces(0).name(), "visible");
}

TEST(WorkspaceServiceAuthorization, UnauthenticatedDeleteIsRejectedWithoutDatabaseWrite) {
    FakeWorkspaceStore store;
    WorkspaceServiceImpl service(&store);
    WorkspaceAuthGuard auth(false, -1);
    rpc::DeleteWorkspaceRequest req;
    rpc::DeleteWorkspaceResponse resp;
    req.set_id(7);

    service.Delete(nullptr, &req, &resp);

    EXPECT_FALSE(resp.success());
    EXPECT_EQ(resp.error_code(), rpc_error::UNAUTHENTICATED);
    EXPECT_EQ(store.delete_calls, 0);
}
