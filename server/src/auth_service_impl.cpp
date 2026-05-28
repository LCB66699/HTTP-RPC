#include "auth_service_impl.h"
#include "database.h"
#include "redis_client.h"
#include "call_logger.h"
#include "system_logger.h"
#include "sha256.h"
#include "jwt.h"
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <openssl/crypto.h>

#ifdef __linux__
#include <termios.h>
#include <unistd.h>
#endif

// Simple base64 encode (no external dep)
static std::string b64enc(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(in.data());
    for (size_t i = 0; i < in.size(); ++i) {
        val = (val << 8) + bytes[i]; valb += 8;
        while (valb >= 0) { out += t[(val >> valb) & 0x3F]; valb -= 6; }
    }
    if (valb > -6) out += t[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
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
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto pos = line.find(':');
        if (pos != std::string::npos)
            users_[line.substr(0, pos)] = line.substr(pos + 1);
    }
}

void AuthServiceImpl::SaveUsers() {
    if (db_) return; // DB saves inline in Register
    std::ofstream f(UsersFilePath());
    for (const auto& [user, pass] : users_)
        f << user << ":" << pass << "\n";
}

grpc::Status AuthServiceImpl::Login(grpc::ServerContext*,
                                     const rpc::LoginRequest* req,
                                     rpc::LoginResponse* resp) {
    auto start = std::chrono::high_resolution_clock::now();

    if (db_) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string stored_hash;
        if (!db_->GetUser(req->username(), stored_hash) ||
            !sha256::verify_password(req->password(), stored_hash)) {
            resp->set_success(false);
            resp->set_error("Invalid username or password");
            if (logger_) {
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now() - start).count();
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
                    std::chrono::high_resolution_clock::now() - start).count();
                json p, r;
                r["error"] = json("Invalid username or password");
                logger_->Log(req->username(), "AuthService", "Login", p, r, false, dur);
            }
            return grpc::Status::OK;
        }
    }

    // Create JWT with 24h expiry + token_version for revocation + uid for DB queries
    int ver = db_ ? db_->GetTokenVersion(req->username()) : 0;
    if (ver < 0) ver = 0;
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
    { std::string pw = req->password(); if (!pw.empty()) OPENSSL_cleanse(&pw[0], pw.size()); }

    resp->set_success(true);
    resp->set_token(token);
    resp->set_user_id(uid);

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p;
        p["username"] = json(req->username());
        json r;
        r["token"] = json("<jwt>");
        logger_->Log(req->username(), "AuthService", "Login", p, r, true, dur);
    }
    if (slog_) LOG_INFO(*slog_, std::string("User '") + req->username() + "' logged in (ver=" + std::to_string(ver) + ")");
    return grpc::Status::OK;
}

grpc::Status AuthServiceImpl::Register(grpc::ServerContext*,
                                        const rpc::RegisterRequest* req,
                                        rpc::RegisterResponse* resp) {
    auto start = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);

    // 先做输入校验（不查重——交给 INSERT 的 UNIQUE 约束，消除 TOCTOU 竞态窗口）
    if (req->username().size() < 3 || req->username().size() > 20) {
        resp->set_success(false);
        resp->set_error("Username must be 3-20 characters");
        if (logger_) {
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - start).count();
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
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - start).count();
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
            resp->set_error(is_dup ? "Username already exists"
                                   : "Registration failed — username may already exist");
            if (logger_) {
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now() - start).count();
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
    if (ver < 0) ver = 0;
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
    { std::string pw = req->password(); if (!pw.empty()) OPENSSL_cleanse(&pw[0], pw.size()); }

    resp->set_success(true);
    resp->set_token(token);
    resp->set_user_id(uid);

    if (logger_) {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
        json p;
        p["username"] = json(req->username());
        json r;
        r["token"] = json("<jwt>");
        logger_->Log(req->username(), "AuthService", "Register", p, r, true, dur);
    }
    if (slog_) LOG_INFO(*slog_, std::string("User '") + req->username() + "' registered");
    return grpc::Status::OK;
}
