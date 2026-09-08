#include <gtest/gtest.h>

#include "auth/sharing_service_impl.h"
#include "shared/base/error_codes.h"
#include "shared/base/rpc_interceptor.h"

class SharingAuthGuard {
   public:
    SharingAuthGuard(bool authenticated, int64_t user_id) : saved_(g_rpc_auth_ctx) {
        g_rpc_auth_ctx = {"caller", user_id, authenticated};
    }
    ~SharingAuthGuard() { g_rpc_auth_ctx = saved_; }

   private:
    AuthContext saved_;
};

TEST(SharingServiceAuthorization, UnauthenticatedCallsAreRejectedBeforeDatabaseAccess) {
    SharingServiceImpl service(nullptr);
    SharingAuthGuard auth(false, 0);
    rpc::ShareLinkRequest request;
    request.set_owner_id(99);
    request.set_resource_type("sheet");
    request.set_resource_id(7);
    request.set_permission("view");
    rpc::ShareLinkResponse response;

    service.CreateShareLink(nullptr, &request, &response);

    EXPECT_FALSE(response.success());
    EXPECT_EQ(response.error_code(), rpc_error::UNAUTHENTICATED);
}

TEST(SharingServiceAuthorization, RequestOwnerCannotImpersonateAuthenticatedPrincipal) {
    SharingServiceImpl service(nullptr);
    SharingAuthGuard auth(true, 7);
    rpc::ShareRequest request;
    request.set_owner_id(99);
    request.set_resource_type("sheet");
    request.set_resource_id(7);
    request.set_grantee_username("target");
    request.set_permission("view");
    rpc::ShareResponse response;

    service.Share(nullptr, &request, &response);

    EXPECT_FALSE(response.success());
    EXPECT_EQ(response.error_code(), rpc_error::FORBIDDEN);
}
