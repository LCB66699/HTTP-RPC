#include <gtest/gtest.h>
#include "../server/include/sha256.h"

TEST(SHA256, HashAndVerify) {
    std::string pw = "test_password_123";
    std::string hash = sha256::hash_password(pw);
    EXPECT_NE(hash, pw); // should not be plaintext
    EXPECT_TRUE(hash.find("pbkdf2_sha256$") == 0); // starts with prefix
    EXPECT_TRUE(sha256::verify_password(pw, hash));
}

TEST(SHA256, WrongPasswordFails) {
    std::string hash = sha256::hash_password("correct");
    EXPECT_FALSE(sha256::verify_password("wrong", hash));
}

TEST(SHA256, EmptyPassword) {
    std::string hash = sha256::hash_password("");
    EXPECT_FALSE(hash.empty());
    EXPECT_TRUE(sha256::verify_password("", hash));
}

TEST(SHA256, HashIsDeterministic) {
    // Same password produces different hashes (random salt)
    std::string h1 = sha256::hash_password("test");
    std::string h2 = sha256::hash_password("test");
    EXPECT_NE(h1, h2); // different salts
    EXPECT_TRUE(sha256::verify_password("test", h1));
    EXPECT_TRUE(sha256::verify_password("test", h2));
}

TEST(SHA256, OldFormatCompat) {
    // Old bare SHA256 format (no $ delimiter)
    std::string oldHash = sha256::hash_hex("oldpassword");
    EXPECT_TRUE(sha256::verify_password("oldpassword", oldHash));
    EXPECT_FALSE(sha256::verify_password("wrong", oldHash));
}

TEST(SHA256, EmptyStoredRejected) {
    EXPECT_FALSE(sha256::verify_password("anything", ""));
}

TEST(SHA256, LongPassword) {
    std::string longPw(1000, 'x');
    std::string hash = sha256::hash_password(longPw);
    EXPECT_TRUE(sha256::verify_password(longPw, hash));
}
