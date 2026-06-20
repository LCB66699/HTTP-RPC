#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "auth/auth_service_impl.h"

// ---- RAII helper: save/restore env var ----
class EnvGuard {
public:
    explicit EnvGuard(const char *name) : name_(name), saved_(std::getenv(name)) {}
    ~EnvGuard() {
        if (saved_) setenv(name_, saved_, 1);
        else unsetenv(name_);
    }
private:
    const char *name_;
    const char *saved_;
};

// ===== IsAdminUser =====

TEST(AuthService, IsAdminUserPrefixMatch) {
    AuthServiceImpl auth("test-secret", nullptr);
    EXPECT_TRUE(auth.IsAdminUser("admin"));
    EXPECT_TRUE(auth.IsAdminUser("admin123"));
    EXPECT_TRUE(auth.IsAdminUser("administrator"));
    EXPECT_FALSE(auth.IsAdminUser("user"));
    EXPECT_FALSE(auth.IsAdminUser("xadmin"));
    EXPECT_FALSE(auth.IsAdminUser(""));
}

TEST(AuthService, IsAdminUserEnvVarSingle) {
    EnvGuard guard("ADMIN_USERS");
    setenv("ADMIN_USERS", "superman", 1);
    AuthServiceImpl auth("test-secret", nullptr);
    EXPECT_TRUE(auth.IsAdminUser("superman"));
    EXPECT_FALSE(auth.IsAdminUser("batman"));
}

TEST(AuthService, IsAdminUserEnvVarMultiple) {
    EnvGuard guard("ADMIN_USERS");
    setenv("ADMIN_USERS", "alpha, beta ,gamma", 1);
    AuthServiceImpl auth("test-secret", nullptr);
    EXPECT_TRUE(auth.IsAdminUser("alpha"));
    EXPECT_TRUE(auth.IsAdminUser("beta"));
    EXPECT_TRUE(auth.IsAdminUser("gamma"));
    EXPECT_FALSE(auth.IsAdminUser("delta"));
}

TEST(AuthService, IsAdminUserEnvVarWithWhitespace) {
    EnvGuard guard("ADMIN_USERS");
    setenv("ADMIN_USERS", "  alice  ,  bob  ", 1);
    AuthServiceImpl auth("test-secret", nullptr);
    EXPECT_TRUE(auth.IsAdminUser("alice"));
    EXPECT_TRUE(auth.IsAdminUser("bob"));
    EXPECT_FALSE(auth.IsAdminUser(" alice"));  // trimmed -> fails
    EXPECT_FALSE(auth.IsAdminUser("alice "));  // trimmed -> fails
}

TEST(AuthService, IsAdminUserEmptyEnvVar) {
    EnvGuard guard("ADMIN_USERS");
    setenv("ADMIN_USERS", "", 1);
    AuthServiceImpl auth("test-secret", nullptr);
    // empty env var + username "user" does NOT match -> no admin via env
    // but "admin" prefix still works
    EXPECT_TRUE(auth.IsAdminUser("admin"));
    EXPECT_FALSE(auth.IsAdminUser("user1"));
}

TEST(AuthService, IsAdminUserNoEnvVar) {
    EnvGuard guard("ADMIN_USERS");
    unsetenv("ADMIN_USERS");
    AuthServiceImpl auth("test-secret", nullptr);
    EXPECT_TRUE(auth.IsAdminUser("admin"));
    EXPECT_FALSE(auth.IsAdminUser("user1"));
}

// ===== b64enc (tested via in-memory Login/Register flow) =====

class AuthServiceLoginTest : public ::testing::Test {
protected:
    void SetUp() override { std::remove("users.json"); }
    void TearDown() override { std::remove("users.json"); }
};

TEST_F(AuthServiceLoginTest, RegisterAndLoginWithCorrectPassword) {
    AuthServiceImpl auth("jwt-secret-12345", nullptr);

    grpc::ServerContext ctx;
    rpc::RegisterRequest req;
    rpc::RegisterResponse resp;
    req.set_username("testuser");
    req.set_password("mypassword");
    auto status = auth.Register(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(resp.success());

    rpc::LoginRequest login_req;
    rpc::LoginResponse login_resp;
    login_req.set_username("testuser");
    login_req.set_password("mypassword");
    status = auth.Login(&ctx, &login_req, &login_resp);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(login_resp.success());
}

TEST_F(AuthServiceLoginTest, LoginWithWrongPassword) {
    AuthServiceImpl auth("jwt-secret-12345", nullptr);

    grpc::ServerContext ctx;
    rpc::RegisterRequest req;
    rpc::RegisterResponse resp;
    req.set_username("testuser2");
    req.set_password("secret123");
    auto status = auth.Register(&ctx, &req, &resp);
    EXPECT_TRUE(resp.success());

    rpc::LoginRequest login_req;
    rpc::LoginResponse login_resp;
    login_req.set_username("testuser2");
    login_req.set_password("wrongpassword");
    status = auth.Login(&ctx, &login_req, &login_resp);
    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(login_resp.success());
}

TEST_F(AuthServiceLoginTest, RegisterDuplicateUsernameFails) {
    AuthServiceImpl auth("jwt-secret-12345", nullptr);

    grpc::ServerContext ctx;
    rpc::RegisterRequest req;
    rpc::RegisterResponse resp;
    req.set_username("dupeuser");
    req.set_password("password123");
    auto status = auth.Register(&ctx, &req, &resp);
    EXPECT_TRUE(resp.success());

    rpc::RegisterRequest req2;
    rpc::RegisterResponse resp2;
    req2.set_username("dupeuser");
    req2.set_password("another456");
    status = auth.Register(&ctx, &req2, &resp2);
    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(resp2.success());
}

TEST_F(AuthServiceLoginTest, RegisterUsernameTooShort) {
    AuthServiceImpl auth("jwt-secret-12345", nullptr);

    grpc::ServerContext ctx;
    rpc::RegisterRequest req;
    rpc::RegisterResponse resp;
    req.set_username("ab");
    req.set_password("password123");
    auto status = auth.Register(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(resp.success());
}

TEST_F(AuthServiceLoginTest, RegisterPasswordTooShort) {
    AuthServiceImpl auth("jwt-secret-12345", nullptr);

    grpc::ServerContext ctx;
    rpc::RegisterRequest req;
    rpc::RegisterResponse resp;
    req.set_username("validuser");
    req.set_password("12345");
    auto status = auth.Register(&ctx, &req, &resp);
    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(resp.success());
}
