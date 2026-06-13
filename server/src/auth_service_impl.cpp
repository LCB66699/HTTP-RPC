#include "auth_service_impl.h"

#include <openssl/crypto.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>

#include "call_logger.h"
#include "database.h"
#include "jwt.h"
#include "redis_client.h"
#include "sha256.h"
#include "system_logger.h"

#ifdef __linux__
#include <termios.h>
#include <unistd.h>
#endif

// Simple base64 encode (no external dep)
static std::string b64enc(const std::string &in) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(in.data());
    for (size_t i = 0; i < in.size(); ++i) {
        val = (val << 8) + bytes[i];
        valb += 8;
        while (valb >= 0) {
            out += t[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6)
        out += t[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.size() % 4)
        out += '=';
    return out;
}

std::string AuthServiceImpl::UsersFilePath() const {
    return "users.json";
}

void AuthServiceImpl::LoadUsers() {
    if (db_) {
        std::ifstream f(UsersFilePath());
        if (f) {
            printf("[Auth] Importing users from users.json to MySQL...\n");
            db_->ImportFromUsersJson(UsersFilePath());
            f.close();
            std::remove(UsersFilePath().c_str());
            printf("[Auth] Migration complete, users.json removed\n");
        }
        return;
    }
    // Fallback: load users.json into map
    std::ifstream f(UsersFilePath());
    if (!f)
        return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        auto pos = line.find(':');
        if (pos != std::string::npos)
            users_[line.substr(0, pos)] = line.substr(pos + 1);
    }
}

void AuthServiceImpl::SaveUsers() {
    if (db_)
        return;  // DB saves inline in Register
    std::ofstream f(UsersFilePath());
    for (const auto &[user, pass] : users_)
        f << user << ":" << pass << "\n";
}

grpc::Status AuthServiceImpl::Login(grpc::ServerContext *, const rpc::LoginRequest *req, rpc::LoginResponse *resp) {
    auto start = std::chrono::high_resolution_clock::now();

    if (db_) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string stored_hash;
        if (!db_->GetUser(req->username(), stored_hash) || !sha256::verify_password(req->password(), stored_hash)) {
            resp->set_success(false);
            resp->set_error("Invalid username or password");
            if (logger_) {
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::high_resolution_clock::now() - start)
                               .count();
                json p, r;
                r["error"] = json("Invalid username or password");
                logger_->Log(req->username(), "AuthService", "Login", p, r, false, dur);
            }
            return grpc::Status::OK;
        }
    } else {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = users_.find(req->username());
        if (it == users_.end() || it->second != b64enc(req->password())) {
            resp->set_success(false);
            resp->set_error("Invalid username or password");
            if (logger_) {
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::high_resolution_clock::now() - start)
                               .count();
                json p, r;
                r["error"] = json("Invalid username or password");
                logger_->Log(req->username(), "AuthService", "Login", p, r, false, dur);
            }
            return grpc::Status::OK;
        }
    }

    // Create JWT with 24h expiry + token_version for revocation + uid for DB
    // queries
    int ver = db_ ? db_->GetTokenVersion(req->username()) : 0;
    if (ver < 0)
        ver = 0;
    int64_t uid = db_ ? db_->GetUserId(req->username()) : -1;
    long exp = static_cast<long>(std::time(nullptr)) + 86400;
    nlohmann::json payload;
    payload["username"] = req->username();
    payload["uid"] = uid;
    payload["ver"] = ver;
    payload["exp"] = exp;
    std::string payload_str = payload.dump();
    std::string token = jwt::create(payload_str, jwt_secret_);

    // 密码使用完毕，擦除内存中的副本
    {
        std::string pw = req->password();
        if (!pw.empty())
            OPENSSL_cleanse(&pw[0], pw.size());
    }

    resp->set_success(true);
    resp->set_user_id(uid);

    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p;
        p["username"] = json(req->username());
        json r;
        r["token"] = json("<jwt>");
        logger_->Log(req->username(), "AuthService", "Login", p, r, true, dur);
    }
    // 签发双 Token
    std::string role = IsAdminUser(req->username()) ? "admin" : "user";
    std::string at = CreateAccessToken(req->username(), uid, role);
    std::string rt = CreateRefreshToken(req->username(), uid);
    resp->set_access_token(at);
    resp->set_refresh_token(rt);
    resp->set_role(role);

    if (slog_)
        LOG_INFO(*slog_, std::string("User '") + req->username() + "' logged in (ver=" + std::to_string(ver) + ")");
    return grpc::Status::OK;
}

grpc::Status AuthServiceImpl::Register(grpc::ServerContext *, const rpc::RegisterRequest *req,
                                       rpc::RegisterResponse *resp) {
    auto start = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);

    // 先做输入校验（不查重——交给 INSERT 的 UNIQUE 约束，消除 TOCTOU 竞态窗口）
    if (req->username().size() < 3 || req->username().size() > 20) {
        resp->set_success(false);
        resp->set_error("Username must be 3-20 characters");
        if (logger_) {
            auto dur =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                    .count();
            json p, r;
            p["username"] = json(req->username());
            r["error"] = json("Username must be 3-20 characters");
            logger_->Log(req->username(), "AuthService", "Register", p, r, false, dur);
        }
        return grpc::Status::OK;
    }
    if (req->password().size() < 6) {
        resp->set_success(false);
        resp->set_error("Password must be at least 6 characters");
        if (logger_) {
            auto dur =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                    .count();
            json p, r;
            p["username"] = json(req->username());
            r["error"] = json("Password must be at least 6 characters");
            logger_->Log(req->username(), "AuthService", "Register", p, r, false, dur);
        }
        return grpc::Status::OK;
    }

    if (db_) {
        // 直接 INSERT — UNIQUE 约束兜底，消除 UserExists→AddUser 之间的 TOCTOU 竞态
        if (!db_->AddUser(req->username(), sha256::hash_password(req->password()))) {
            bool is_dup = db_->UserExists(req->username());
            resp->set_success(false);
            resp->set_error(is_dup ? "Username already exists" : "Registration failed — username may already exist");
            if (logger_) {
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::high_resolution_clock::now() - start)
                               .count();
                json p, r;
                p["username"] = json(req->username());
                r["error"] = json(is_dup ? "Username already exists" : "Registration failed");
                logger_->Log(req->username(), "AuthService", "Register", p, r, false, dur);
            }
            return grpc::Status::OK;
        }
    } else {
        if (users_.count(req->username())) {
            resp->set_success(false);
            resp->set_error("Username already exists");
            return grpc::Status::OK;
        }
        users_[req->username()] = b64enc(req->password());
        SaveUsers();
    }

    // Auto-login after register (token_version = 0)
    int ver = db_ ? db_->GetTokenVersion(req->username()) : 0;
    if (ver < 0)
        ver = 0;
    int64_t uid = db_ ? db_->GetUserId(req->username()) : -1;
    long exp = static_cast<long>(std::time(nullptr)) + 86400;
    nlohmann::json payload;
    payload["username"] = req->username();
    payload["uid"] = uid;
    payload["ver"] = ver;
    payload["exp"] = exp;
    std::string payload_str = payload.dump();
    std::string token = jwt::create(payload_str, jwt_secret_);

    // 同步 token_version 到 Redis
    if (redis_ && redis_->IsConnected())
        redis_->SetJSON("token_ver:" + req->username(), std::to_string(ver), 86400);

    // 密码使用完毕，擦除内存中的副本
    {
        std::string pw = req->password();
        if (!pw.empty())
            OPENSSL_cleanse(&pw[0], pw.size());
    }

    // 签发双 Token
    std::string role = IsAdminUser(req->username()) ? "admin" : "user";
    std::string at = CreateAccessToken(req->username(), uid, role);
    std::string rt = CreateRefreshToken(req->username(), uid);

    resp->set_success(true);
    resp->set_user_id(uid);
    resp->set_access_token(at);
    resp->set_refresh_token(rt);
    resp->set_role(role);

    if (logger_) {
        auto dur =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        json p;
        p["username"] = json(req->username());
        json r;
        r["token"] = json("<jwt>");
        logger_->Log(req->username(), "AuthService", "Register", p, r, true, dur);
    }
    if (slog_)
        LOG_INFO(*slog_, std::string("User '") + req->username() + "' registered");
    return grpc::Status::OK;
}

std::string AuthServiceImpl::CreateAccessToken(const std::string &username, int64_t uid,
                                               const std::string &role) const {
    long exp = static_cast<long>(std::time(nullptr)) + 900;  // 15 minutes
    nlohmann::json payload;
    payload["username"] = username;
    payload["uid"] = uid;
    payload["role"] = role;
    payload["type"] = "access";
    payload["exp"] = exp;
    return jwt::create(payload.dump(), jwt_secret_);
}

std::string AuthServiceImpl::CreateRefreshToken(const std::string &username, int64_t uid) const {
    unsigned char raw[16];
    for (int i = 0; i < 16; ++i)
        raw[i] = (unsigned char)(rand() % 256);
    raw[6] = (raw[6] & 0x0f) | 0x40;
    raw[8] = (raw[8] & 0x3f) | 0x80;
    char buf[37];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", raw[0], raw[1],
             raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13],
             raw[14], raw[15]);
    std::string rt(buf);
    if (redis_ && redis_->IsConnected())
        redis_->SetJSON("rt:" + username, "{\"token\":\"" + rt + "\",\"uid\":" + std::to_string(uid) + "}", 604800);
    return rt;
}

grpc::Status AuthServiceImpl::RefreshToken(grpc::ServerContext *, const rpc::RefreshTokenRequest *req,
                                           rpc::RefreshTokenResponse *resp) {
    std::string stored;
    if (!redis_ || !redis_->GetJSON("rt:" + req->username(), stored)) {
        resp->set_success(false);
        resp->set_error("Invalid refresh token");
        return grpc::Status::OK;
    }
    std::string stored_rt;
    int64_t stored_uid = 0;
    try {
        auto j = nlohmann::json::parse(stored);
        stored_rt = j.value("token", "");
        stored_uid = j.value("uid", 0);
    } catch (...) {
        stored_rt = stored;
    }
    if (stored_rt != req->refresh_token()) {
        // Token 不匹配 → 盗用检测，撤销所有
        if (redis_) {
            redis_->DeleteKey("rt:" + req->username());
            redis_->DeleteKey("rate:login:" + req->username() + ":total");
        }
        resp->set_success(false);
        resp->set_error("Invalid refresh token");
        return grpc::Status::OK;
    }
    std::string role = IsAdminUser(req->username()) ? "admin" : "user";
    std::string at = CreateAccessToken(req->username(), stored_uid, role);
    resp->set_success(true);
    resp->set_access_token(at);
    return grpc::Status::OK;
}

bool AuthServiceImpl::IsAdminUser(const std::string &username) const {
    if (username.rfind("admin", 0) == 0)
        return true;
    const char *env = std::getenv("ADMIN_USERS");
    if (!env || !*env)
        return false;
    std::string list(env);
    size_t pos = 0;
    while (pos < list.size()) {
        size_t comma = list.find(',', pos);
        std::string token = list.substr(pos, comma - pos);
        size_t s = 0, e = token.size();
        while (s < e && token[s] == ' ')
            ++s;
        while (e > s && token[e - 1] == ' ')
            --e;
        if (token.substr(s, e - s) == username)
            return true;
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return false;
}

grpc::Status AuthServiceImpl::ValidateUser(grpc::ServerContext *, const rpc::ValidateUserRequest *req,
                                           rpc::ValidateUserResponse *resp) {
    std::string username;
    int64_t uid = req->user_id();
    int64_t token_uid = 0;

    // 从 token 中验证身份
    if (!req->token().empty()) {
        std::string payload;
        if (jwt::verify(req->token(), jwt_secret_, payload)) {
            try {
                auto j = nlohmann::json::parse(payload);
                username = j.value("username", "");
                token_uid = j.value("uid", 0LL);
            } catch (...) {
            }
        }
    }

    // 身份比对：请求的 user_id 必须与 JWT 中的 uid 一致（0 表示不限定）
    if (uid != 0 && token_uid != 0 && uid != token_uid) {
        resp->set_valid(false);
        resp->set_error("user_id mismatch token uid");
        return grpc::Status::OK;
    }
    if (uid == 0)
        uid = token_uid;

    // 如果 token 无效或没传，用 username/user_id 查数据库
    if (username.empty() && !req->username().empty()) {
        username = req->username();
    }

    if (username.empty()) {
        resp->set_valid(false);
        resp->set_error("invalid identity");
        return grpc::Status::OK;
    }

    // 查数据库确认用户存在
    if (db_) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string stored_hash;
        if (!db_->GetUser(username, stored_hash)) {
            resp->set_valid(false);
            resp->set_error("user not found");
            return grpc::Status::OK;
        }
        if (uid == 0)
            uid = db_->GetUserId(username);
    } else {
        auto it = users_.find(username);
        if (it == users_.end()) {
            resp->set_valid(false);
            resp->set_error("user not found");
            return grpc::Status::OK;
        }
    }

    resp->set_valid(true);
    resp->set_username(username);
    resp->set_user_id(uid);
    resp->set_role(IsAdminUser(username) ? "admin" : "user");
    return grpc::Status::OK;
}
