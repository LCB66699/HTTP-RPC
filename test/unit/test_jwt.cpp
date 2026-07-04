#include <gtest/gtest.h>
#include "shared/client/jwt.h"

TEST(JWT, CreateAndVerify) {
    std::string secret = "test-secret-32bytes-here-abcdef!";
    std::string payload = R"({"username":"tester","exp":9999999999})";
    std::string token = jwt::create(payload, secret);
    EXPECT_FALSE(token.empty());
    std::string decoded;
    EXPECT_TRUE(jwt::verify(token, secret, decoded));
    EXPECT_TRUE(decoded.find("tester") != std::string::npos);
}

TEST(JWT, WrongSecretRejected) {
    std::string token = jwt::create(R"({"sub":"test"})", "secret-a");
    std::string decoded;
    EXPECT_FALSE(jwt::verify(token, "secret-b", decoded));
}

TEST(JWT, TamperedToken) {
    std::string token = jwt::create(R"({"sub":"test"})", "secret");
    token[token.size() - 1] ^= 1;  // flip last byte
    std::string decoded;
    EXPECT_FALSE(jwt::verify(token, "secret", decoded));
}

TEST(JWT, EmptySecret) {
    std::string token = jwt::create("{}", "");
    EXPECT_FALSE(token.empty());
}

TEST(JWT, Base64UrlFormat) {
    std::string token = jwt::create(R"({"x":"y"})", "s3cret!");
    // JWT has 3 dot-separated parts
    int dots = 0;
    for (char c : token) if (c == '.') dots++;
    EXPECT_EQ(dots, 2);
}
