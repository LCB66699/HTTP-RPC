#include "gateway.h"
#include "server/include/jwt.h"
#include "http2_server.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "server/include/httplib.h"
#include <cstdio>
#include <ctime>
#include <chrono>
#include <thread>
#include <sstream>

// gRPC channel 配置 (keepalive + round_robin + retry)
// DNS aliases 返回多个 IP → gRPC 内置 round_robin 在 Subchannel 间自动轮询
static std::shared_ptr<grpc::Channel> MakeChannel(const std::string& addr) {
    // Create operations are NOT retried at the gRPC level — idempotency is handled
    // by the client-supplied idempotency_key + server-side ON DUPLICATE KEY instead.
    // All other methods (reads + Update/Delete) may retry once on UNAVAILABLE.
    static const char* kRetryPolicy = R"({
      "methodConfig": [
        {
          "name": [
            {"service": "rpc.SpreadsheetService", "method": "CreateSpreadsheet"},
            {"service": "rpc.FileService",         "method": "CreateFile"}
          ]
        },
        {
          "name": [{}],
          "retryPolicy": {
            "maxAttempts": 3,
            "initialBackoff": "0.1s",
            "maxBackoff": "5s",
            "backoffMultiplier": 2,
            "retryableStatusCodes": ["UNAVAILABLE"]
          }
        }
      ]
    })";
    grpc::ChannelArguments args;
    args.SetLoadBalancingPolicyName("round_robin");
    args.SetServiceConfigJSON(kRetryPolicy);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 60000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    args.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, 64 * 1024 * 1024);
    args.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, 64 * 1024 * 1024);
    args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, 5000);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 500);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 10000);
    return grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);
}

// Per-replica-aware gRPC call: retries once if the first attempt lands on a quarantined
// replica, giving gRPC round_robin a chance to pick a different subchannel.
// rpc_fn(ctx_ptr) → std::pair<grpc::Status, bool> where bool = rpc_ok && resp.success()
template<typename F>
static bool RepRetry(PerReplicaTracker& rep, const std::string& username,
                     const std::string& raw_token, int timeout_sec,
                     F&& rpc_fn, std::string& out_peer) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        grpc::ClientContext ctx;
        ctx.AddMetadata("username", username);
        ctx.AddMetadata("authorization", "Bearer " + raw_token);
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(timeout_sec));
        auto [st, ok] = rpc_fn(&ctx);
        out_peer = ctx.peer();
        if (ok) { rep.RecordSuccess(out_peer); return true; }
        rep.RecordFailure(out_peer);
        if (rep.AllowReplica(out_peer)) return false; // not quarantined, give up
    }
    return false;
}

Gateway::Gateway(const std::string& listen_addr, int listen_port,
                 const std::string& grpc_auth_addr,
                 const std::string& grpc_sheet_addr,
                 const std::string& grpc_file_addr,
                 const std::string& jwt_secret,
                 const std::string& web_dir,
                 const std::vector<std::string>& redis_cluster_seeds,
                 const std::string& redis_password,
                 int redis_pool_size)
    : listen_addr_(listen_addr), listen_port_(listen_port),
      grpc_auth_addr_(grpc_auth_addr), grpc_sheet_addr_(grpc_sheet_addr), grpc_file_addr_(grpc_file_addr),
      jwt_secret_(jwt_secret), web_dir_(web_dir),
      redis_cluster_seeds_(redis_cluster_seeds),
      redis_password_(redis_password), redis_pool_size_(redis_pool_size)
{
    // 每个服务一个 Channel，DNS别名返回多IP → gRPC round_robin 自动负载均衡
    if (!grpc_auth_addr_.empty()) {
        auth_ch_ = MakeChannel(grpc_auth_addr_);
        auth_stub_ = rpc::AuthService::NewStub(auth_ch_);
    }
    if (!grpc_sheet_addr_.empty()) {
        sheet_ch_ = MakeChannel(grpc_sheet_addr_);
        sheet_stub_ = rpc::SpreadsheetService::NewStub(sheet_ch_);
    }
    if (!grpc_file_addr_.empty()) {
        file_ch_ = MakeChannel(grpc_file_addr_);
        file_stub_ = rpc::FileService::NewStub(file_ch_);
    }
    if (!grpc_sheet_addr_.empty()) {
        health_ch_ = MakeChannel(grpc_sheet_addr_);
        health_stub_ = rpc::HealthMonitor::NewStub(health_ch_);
    }
    printf("[Gateway] gRPC channels ready (DNS round_robin)\n");
    printf("[Gateway] TM deferred (MySQL may not be ready yet)\n");
}

// JSON helpers (powered by nlohmann/json)
std::string Gateway::JsonStr(const std::string& s) {
    return nlohmann::json(s).dump();
}
std::string Gateway::JsonGet(const std::string& json_str, const std::string& key) {
    try {
        auto j = nlohmann::json::parse(json_str);
        auto it = j.find(key);
        if (it == j.end()) return "";
        if (it->is_string()) return it->get<std::string>();
        if (it->is_number()) return std::to_string(it->get<double>());
        if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
        return it->dump();
    } catch (...) { return ""; }
}
double Gateway::JsonGetNum(const std::string& json_str, const std::string& key) {
    try {
        auto j = nlohmann::json::parse(json_str);
        auto it = j.find(key);
        if (it == j.end()) return 0;
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) { try { return std::stod(it->get<std::string>()); } catch (...) { return 0; } }
        return 0;
    } catch (...) { return 0; }
}

std::string Gateway::CreateJWT(const std::string& username) const {
    long exp = static_cast<long>(std::time(nullptr)) + 86400;
    return jwt::create("{\"username\":" + JsonStr(username) + ",\"exp\":" + std::to_string(exp) + "}", jwt_secret_);
}

// ---- 双 Token: Access Token (JWT 15min) + Refresh Token (UUID 7d) ----

std::string Gateway::CreateAccessToken(const std::string& username) const {
    long exp = static_cast<long>(std::time(nullptr)) + 900;  // 15 minutes
    nlohmann::json payload;
    payload["username"] = username;
    payload["uid"] = 0;         // uid from JWT, set on refresh
    payload["type"] = "access";
    payload["exp"] = exp;
    return jwt::create(payload.dump(), jwt_secret_);
}

std::string Gateway::CreateRefreshToken(const std::string& username) const {
    unsigned char raw[16];
    for (int i = 0; i < 16; ++i) raw[i] = (unsigned char)(rand() % 256);
    raw[6] = (raw[6] & 0x0f) | 0x40;
    raw[8] = (raw[8] & 0x3f) | 0x80;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7],
        raw[8],raw[9],raw[10],raw[11],raw[12],raw[13],raw[14],raw[15]);
    std::string rt(buf);
    if (redis_ && redis_->IsConnected())
        redis_->SetJSON("rt:" + username, rt, 604800);  // 7 days
    return rt;
}

// AT 验证: 仅验签+过期, 不查 Redis token_ver — 15min 短过期天然防吊销延迟
bool Gateway::VerifyAccessToken(const std::string& cookie_header,
                                 std::string& username, int64_t& user_id) const {
    const std::string needle = "rpc_at=";
    size_t pos = cookie_header.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    size_t end = cookie_header.find(';', pos);
    std::string token = cookie_header.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (token.empty()) return false;

    std::string payload;
    if (!jwt::verify(token, jwt_secret_, payload)) return false;

    // 验 type 字段
    if (JsonGet(payload, "type") != "access") return false;

    long exp = (long)JsonGetNum(payload, "exp");
    if (exp == 0 || exp < (long)std::time(nullptr)) return false;

    username = JsonGet(payload, "username");
    if (username.empty()) return false;
    user_id = static_cast<int64_t>(JsonGetNum(payload, "uid"));
    return true;
}

// 向后兼容: VerifyAuth → VerifyAccessToken, 忽略 raw_token
bool Gateway::VerifyAuth(const std::string& cookie_header, std::string& username,
                          int64_t& user_id, std::string& raw_token) const {
    if (VerifyAccessToken(cookie_header, username, user_id)) {
        raw_token = "";  // AT 不再通过 gRPC metadata 传递吊销参数
        return true;
    }
    return false;
}

// Auth handlers
// at_out: Access Token JWT → Set-Cookie HttpOnly (15min)
// rt_out: Refresh Token UUID → response body _rt field (7d)
RpcResult Gateway::HandleLogin(const std::string& body, std::string& response,
                                std::string& at_out, std::string& rt_out) {
    std::string u = JsonGet(body, "username"), p = JsonGet(body, "password");
    if (u.empty() || p.empty()) { response = R"({"success":false,"error":"Username and password required"})"; return RpcResult::BAD_REQUEST; }
    grpc::ClientContext ctx; ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    rpc::LoginRequest req; rpc::LoginResponse resp;
    req.set_username(u); req.set_password(p);
    auto st = auth_stub_->Login(&ctx, req, &resp);
    bool ok = st.ok() && resp.success();
    if (ok) {
        at_out = CreateAccessToken(u);
        rt_out = CreateRefreshToken(u);
        nlohmann::json r;
        r["success"] = true; r["username"] = u; r["_rt"] = rt_out; response = r.dump();
    } else {
        nlohmann::json r; r["success"] = false; r["error"] = resp.error(); response = r.dump();
    }
    return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;
}

// ---- 协程版 Login（token 由调用方通过 Set-Cookie 下发，此处仅返回 username）----
Task<void> Gateway::HandleLoginCoro(std::string body, std::string& response) {
    std::string u = JsonGet(body, "username"), p = JsonGet(body, "password");
    if (u.empty() || p.empty()) {
        response = "{\"success\":false,\"error\":\"Username and password required\"}";
        co_return;
    }
    rpc::LoginRequest req; req.set_username(u); req.set_password(p);
    auto r = co_await GrpcCallOf(
        auth_cq_.cq(), auth_stub_.get(),
        &rpc::AuthService::Stub::AsyncLogin, req, 5);
    bool ok = r.st.ok() && r.resp.success();
    // Coro path: token cannot be set as cookie here without access to the response object.
    // The h2c lambda must intercept the token; for now forward it in body so the h2c lambda can set the cookie.
    response = ok
        ? "{\"success\":true,\"username\":" + JsonStr(u) + ",\"_tok\":" + JsonStr(r.resp.token()) + "}"
        : "{\"success\":false,\"error\":" + JsonStr(r.resp.error()) + "}";
    co_return;
}

RpcResult Gateway::HandleRegister(const std::string& body, std::string& response,
                                   std::string& at_out, std::string& rt_out) {
    std::string u = JsonGet(body, "username"), p = JsonGet(body, "password");
    if (u.empty() || p.empty()) { response = R"({"success":false,"error":"Username and password required"})"; return RpcResult::BAD_REQUEST; }
    grpc::ClientContext ctx; ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    rpc::RegisterRequest req; rpc::RegisterResponse resp;
    req.set_username(u); req.set_password(p);
    auto st = auth_stub_->Register(&ctx, req, &resp);
    bool ok = st.ok() && resp.success();
    if (ok) {
        at_out = CreateAccessToken(u);
        rt_out = CreateRefreshToken(u);
        nlohmann::json r;
        r["success"] = true; r["username"] = u; r["_rt"] = rt_out; response = r.dump();
    } else {
        nlohmann::json r; r["success"] = false; r["error"] = resp.error(); response = r.dump();
    }
    return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;
}

RpcResult Gateway::HandleServices(std::string& response) {
    response = R"({"services":[)"
        R"({"name":"SpreadsheetService","description":"Data spreadsheet storage - write MySQL, read Redis","methods":[)"
            R"({"name":"Create","params":[...]},)"
            R"({"name":"Get","params":[{"name":"id","type":"int"}]},)"
            R"({"name":"List","params":[]},)"
            R"({"name":"Update","params":[...]},)"
            R"({"name":"Delete","params":[{"name":"id","type":"int"}]}]},)"
        R"({"name":"FileService","description":"File storage and download","methods":[)"
            R"({"name":"Upload","params":[]},)"
            R"({"name":"List","params":[]},)"
            R"({"name":"Download","params":[{"name":"id","type":"int"}]},)"
            R"({"name":"Delete","params":[{"name":"id","type":"int"}]}]}])";
    return RpcResult::SUCCESS;
}

// Sheet handlers
RpcResult Gateway::HandleSheetCreate(const std::string& username, int64_t user_id,
                                     const std::string& body, const std::string& idempotency_key,
                                     const std::string& raw_token, PerReplicaTracker& rep,
                                     std::string& response) {
    std::string name = JsonGet(body, "name");
    if (name.empty()) { response = "{\"success\":false,\"error\":\"name is required\"}"; return RpcResult::BAD_REQUEST; }
    rpc::CreateSpreadsheetRequest req; rpc::CreateSpreadsheetResponse resp;
    req.set_user_id(user_id); req.set_name(name);
    req.set_description(JsonGet(body, "description"));
    req.set_headers_json(JsonGet(body, "headers_json"));
    req.set_data_json(JsonGet(body, "data_json"));
    req.set_idempotency_key(idempotency_key);
    std::string peer;
    bool ok = RepRetry(rep, username, raw_token, 5,
        [&](grpc::ClientContext* ctx) {
            auto st = sheet_stub_->CreateSpreadsheet(ctx, req, &resp);
            return std::pair{st, st.ok() && resp.success()};
        }, peer);
    if (ok) {
        nlohmann::json r; r["success"] = true; r["id"] = resp.id(); response = r.dump();
    } else {
        response = R"({"success":false,"error":"Failed"})";
    }
    return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;
}

// 协程版 Sheet Create
Task<void> Gateway::HandleSheetCreateCoro(std::string username, int64_t user_id, std::string body,
                                          std::string idempotency_key, std::string& response) {
    std::string name = JsonGet(body, "name");
    if (name.empty()) { response = "{\"success\":false,\"error\":\"name is required\"}"; co_return; }
    rpc::CreateSpreadsheetRequest req;
    req.set_user_id(user_id); req.set_name(name);
    req.set_description(JsonGet(body, "description"));
    req.set_headers_json(JsonGet(body, "headers_json"));
    req.set_data_json(JsonGet(body, "data_json"));
    req.set_idempotency_key(idempotency_key);
    auto r = co_await GrpcCallOf(
        sheet_cq_.cq(), sheet_stub_.get(),
        &rpc::SpreadsheetService::Stub::AsyncCreateSpreadsheet, req, 5);
    bool ok = r.st.ok() && r.resp.success();
    if (ok) {
        nlohmann::json j; j["success"] = true; j["id"] = r.resp.id(); response = j.dump();
    } else {
        response = R"({"success":false,"error":"Failed"})";
    }
    co_return;
}

// 协程版 Sheet Get
Task<void> Gateway::HandleSheetGetCoro(std::string username, int64_t user_id,
                                        std::string body, std::string& response) {
    int64_t id = static_cast<int64_t>(JsonGetNum(body, "id"));
    rpc::GetSpreadsheetRequest req; req.set_id(id); req.set_user_id(user_id);
    auto r = co_await GrpcCallOf(
        sheet_cq_.cq(), sheet_stub_.get(),
        &rpc::SpreadsheetService::Stub::AsyncGetSpreadsheet, req, 5);
    if (!r.st.ok() || !r.resp.success()) { response = "{\"success\":false}"; co_return; }
    const auto& s = r.resp.spreadsheet();
    response = "{\"success\":true,\"cache_source\":" + JsonStr(r.resp.cache_source())
        + ",\"spreadsheet\":{\"id\":" + std::to_string(s.id())
        + ",\"name\":" + JsonStr(s.name())
        + ",\"row_count\":" + std::to_string(s.row_count())
        + ",\"updated_at\":" + JsonStr(s.updated_at()) + "}}";
    co_return;
}

// 协程版 Sheet List
Task<void> Gateway::HandleSheetListCoro(std::string username, int64_t user_id,
                                         int page, int page_size, std::string& response) {
    rpc::ListSpreadsheetsRequest req;
    req.set_user_id(user_id);
    req.set_page(page);
    req.set_page_size(page_size);
    auto r = co_await GrpcCallOf(
        sheet_cq_.cq(), sheet_stub_.get(),
        &rpc::SpreadsheetService::Stub::AsyncListSpreadsheets, req, 5);
    if (!r.st.ok() || !r.resp.success()) { response = "{\"sheets\":[],\"total\":0}"; co_return; }
    std::string arr = "[";
    for (int i = 0; i < r.resp.sheets_size(); ++i) {
        if (i > 0) arr += ",";
        const auto& s = r.resp.sheets(i);
        arr += "{\"id\":" + std::to_string(s.id()) + ",\"name\":" + JsonStr(s.name())
            + ",\"description\":" + JsonStr(s.description())
            + ",\"row_count\":" + std::to_string(s.row_count())
            + ",\"col_count\":" + std::to_string(s.col_count())
            + ",\"updated_at\":" + JsonStr(s.updated_at()) + "}";
    }
    arr += "]";
    response = "{\"success\":true,\"cache_source\":" + JsonStr(r.resp.cache_source())
        + ",\"total\":" + std::to_string(r.resp.total()) + ",\"sheets\":" + arr + "}";
    co_return;
}

RpcResult Gateway::HandleSheetGet(const std::string& username, int64_t user_id,
                                   const std::string& body, const std::string& raw_token,
                                   PerReplicaTracker& rep, std::string& response) {
    int64_t id = static_cast<int64_t>(JsonGetNum(body, "id"));
    rpc::GetSpreadsheetRequest req; rpc::GetSpreadsheetResponse resp;
    req.set_id(id); req.set_user_id(user_id);
    std::string peer;
    bool ok = RepRetry(rep, username, raw_token, 5,
        [&](grpc::ClientContext* ctx) {
            auto st = sheet_stub_->GetSpreadsheet(ctx, req, &resp);
            return std::pair{st, st.ok() && resp.success()};
        }, peer);
    if (!ok) { response = "{\"success\":false}"; return RpcResult::TRANSPORT_FAILURE; }
    const auto& s = resp.spreadsheet();
    response = "{\"success\":true,\"cache_source\":" + JsonStr(resp.cache_source())
        + ",\"spreadsheet\":{\"id\":" + std::to_string(s.id())
        + ",\"name\":" + JsonStr(s.name())
        + ",\"description\":" + JsonStr(s.description())
        + ",\"headers_json\":" + s.headers_json()
        + ",\"data_json\":" + s.data_json()
        + ",\"row_count\":" + std::to_string(s.row_count())
        + ",\"col_count\":" + std::to_string(s.col_count())
        + ",\"created_at\":" + JsonStr(s.created_at())
        + ",\"updated_at\":" + JsonStr(s.updated_at()) + "}}";
    return RpcResult::SUCCESS;
}

RpcResult Gateway::HandleSheetList(const std::string& username, int64_t user_id,
                                    int page, int page_size, const std::string& raw_token,
                                    PerReplicaTracker& rep, std::string& response) {
    rpc::ListSpreadsheetsRequest req; rpc::ListSpreadsheetsResponse resp;
    req.set_user_id(user_id);
    req.set_page(page);
    req.set_page_size(page_size);
    std::string peer;
    bool ok = RepRetry(rep, username, raw_token, 5,
        [&](grpc::ClientContext* ctx) {
            auto st = sheet_stub_->ListSpreadsheets(ctx, req, &resp);
            return std::pair{st, st.ok() && resp.success()};
        }, peer);
    if (!ok) { response = "{\"sheets\":[],\"total\":0}"; return RpcResult::TRANSPORT_FAILURE; }
    std::string arr = "[";
    for (int i = 0; i < resp.sheets_size(); ++i) {
        if (i > 0) arr += ","; const auto& s = resp.sheets(i);
        arr += "{\"id\":" + std::to_string(s.id()) + ",\"name\":" + JsonStr(s.name())
            + ",\"description\":" + JsonStr(s.description())
            + ",\"row_count\":" + std::to_string(s.row_count())
            + ",\"col_count\":" + std::to_string(s.col_count())
            + ",\"updated_at\":" + JsonStr(s.updated_at()) + "}";
    }
    arr += "]";
    response = "{\"success\":true,\"cache_source\":" + JsonStr(resp.cache_source())
        + ",\"total\":" + std::to_string(resp.total()) + ",\"sheets\":" + arr + "}";
    return RpcResult::SUCCESS;
}

RpcResult Gateway::HandleSheetUpdate(const std::string& username, int64_t user_id,
                                      const std::string& body, const std::string& raw_token,
                                      PerReplicaTracker& rep, std::string& response) {
    int64_t id = static_cast<int64_t>(JsonGetNum(body, "id"));
    rpc::UpdateSpreadsheetRequest req; rpc::UpdateSpreadsheetResponse resp;
    req.set_id(id); req.set_user_id(user_id);
    req.set_name(JsonGet(body, "name")); req.set_description(JsonGet(body, "description"));
    req.set_headers_json(JsonGet(body, "headers_json")); req.set_data_json(JsonGet(body, "data_json"));
    std::string peer;
    bool ok = RepRetry(rep, username, raw_token, 5,
        [&](grpc::ClientContext* ctx) {
            auto st = sheet_stub_->UpdateSpreadsheet(ctx, req, &resp);
            return std::pair{st, st.ok() && resp.success()};
        }, peer);
    response = ok ? "{\"success\":true}" : "{\"success\":false}";
    return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;
}

RpcResult Gateway::HandleSheetDelete(const std::string& username, int64_t user_id,
                                      const std::string& body, const std::string& raw_token,
                                      PerReplicaTracker& rep, std::string& response) {
    int64_t id = static_cast<int64_t>(JsonGetNum(body, "id"));
    rpc::DeleteSpreadsheetRequest req; rpc::DeleteSpreadsheetResponse resp;
    req.set_id(id); req.set_user_id(user_id);
    std::string peer;
    bool ok = RepRetry(rep, username, raw_token, 5,
        [&](grpc::ClientContext* ctx) {
            auto st = sheet_stub_->DeleteSpreadsheet(ctx, req, &resp);
            return std::pair{st, st.ok() && resp.success()};
        }, peer);
    response = ok ? "{\"success\":true}" : "{\"success\":false}";
    return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;
}

// File handlers
RpcResult Gateway::HandleFileList(const std::string& username, int64_t user_id,
                                   int page, int page_size, const std::string& raw_token,
                                   PerReplicaTracker& rep, std::string& response) {
    rpc::ListFilesRequest req; rpc::ListFilesResponse resp;
    req.set_user_id(user_id);
    req.set_page(page);
    req.set_page_size(page_size);
    std::string peer;
    bool ok = RepRetry(rep, username, raw_token, 5,
        [&](grpc::ClientContext* ctx) {
            auto st = file_stub_->ListFiles(ctx, req, &resp);
            return std::pair{st, st.ok() && resp.success()};
        }, peer);
    if (!ok) { response = "{\"files\":[],\"total\":0}"; return RpcResult::TRANSPORT_FAILURE; }
    std::string arr = "[";
    for (int i = 0; i < resp.files_size(); ++i) {
        if (i > 0) arr += ","; const auto& f = resp.files(i);
        arr += "{\"id\":" + std::to_string(f.id()) + ",\"original_name\":" + JsonStr(f.original_name())
            + ",\"size\":" + std::to_string(f.size()) + ",\"mime_type\":" + JsonStr(f.mime_type())
            + ",\"created_at\":" + JsonStr(f.created_at()) + "}";
    }
    arr += "]";
    response = "{\"success\":true,\"total\":" + std::to_string(resp.total()) + ",\"files\":" + arr + "}";
    return RpcResult::SUCCESS;
}

RpcResult Gateway::HandleFileDelete(const std::string& username, int64_t user_id,
                                     const std::string& body, const std::string& raw_token,
                                     PerReplicaTracker& rep, std::string& response) {
    int64_t id = static_cast<int64_t>(JsonGetNum(body, "id"));
    rpc::DeleteFileRequest req; rpc::DeleteFileResponse resp;
    req.set_id(id); req.set_user_id(user_id);
    std::string peer;
    bool ok = RepRetry(rep, username, raw_token, 5,
        [&](grpc::ClientContext* ctx) {
            auto st = file_stub_->DeleteFile(ctx, req, &resp);
            return std::pair{st, st.ok() && resp.success()};
        }, peer);
    response = ok ? "{\"success\":true}" : "{\"success\":false}";
    return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;
}

RpcResult Gateway::HandleTxBegin(const std::string&, const std::string& body, std::string& response) {
    if (!tx_manager_) { response = "{\"error\":\"TM disabled\"}"; return RpcResult::BUSINESS_FAILURE; }
    std::string xid = JsonGet(body, "xid");
    int timeout = static_cast<int>(JsonGetNum(body, "timeout"));
    if (timeout <= 0) timeout = 30;
    std::vector<rpc::TxOperation> ops;
    std::string error;
    bool ok = tx_manager_->Begin(xid, ops, timeout, error);
    response = ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"" + error + "\"}";
    return ok ? RpcResult::SUCCESS : RpcResult::TRANSPORT_FAILURE;
}

bool Gateway::WaitForBackends(int timeout_sec) {
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_sec);
    if (auth_ch_    && !auth_ch_->WaitForConnected(deadline))  { fprintf(stderr, "[Gateway] auth backend timeout\n");  return false; }
    if (sheet_ch_   && !sheet_ch_->WaitForConnected(deadline)) { fprintf(stderr, "[Gateway] sheet backend timeout\n"); return false; }
    if (file_ch_    && !file_ch_->WaitForConnected(deadline))  { fprintf(stderr, "[Gateway] file backend timeout\n");  return false; }
    printf("[Gateway] All backends connected\n");
    return true;
}

RpcResult Gateway::HandleHealth(std::string& response) {
    auto state_str = [](grpc_connectivity_state s) -> const char* {
        switch (s) {
            case GRPC_CHANNEL_READY:              return "READY";
            case GRPC_CHANNEL_TRANSIENT_FAILURE:  return "FAILED";
            case GRPC_CHANNEL_CONNECTING:         return "CONNECTING";
            case GRPC_CHANNEL_IDLE:               return "IDLE";
            default:                               return "SHUTDOWN";
        }
    };
    response = std::string("{")
        + "\"auth\":{\"channel\":\"" + state_str(auth_ch_ ? auth_ch_->GetState(true) : GRPC_CHANNEL_SHUTDOWN)
        + "\",\"breaker\":\"" + cb_auth_.StateStr() + "\"},"
        + "\"sheet\":{\"channel\":\"" + state_str(sheet_ch_ ? sheet_ch_->GetState(true) : GRPC_CHANNEL_SHUTDOWN)
        + "\",\"breaker\":\"" + cb_sheet_.StateStr() + "\"},"
        + "\"file\":{\"channel\":\"" + state_str(file_ch_ ? file_ch_->GetState(true) : GRPC_CHANNEL_SHUTDOWN)
        + "\",\"breaker\":\"" + cb_file_.StateStr() + "\"},"
        + "\"gateway\":\"READY\""
        + "}";
    return RpcResult::SUCCESS;
}

RpcResult Gateway::HandleHistory(std::string& response) {
    if (!redis_ || !redis_->IsConnected()) { response = "{\"history\":[],\"total\":0}"; return RpcResult::BUSINESS_FAILURE; }
    // Empty username → read from call_history:global (aggregate of all users)
    auto entries = redis_->GetCallEntries(50, 0, "");
    std::string arr = "[";
    for (size_t i = 0; i < entries.size(); ++i) { if (i > 0) arr += ","; arr += entries[i]; }
    arr += "]";
    response = "{\"history\":" + arr + ",\"total\":" + std::to_string(redis_->GetCallCount("")) + "}";
    return RpcResult::SUCCESS;
}

bool Gateway::HandleSystemStatus(std::string& response) {
    auto state_str = [](grpc_connectivity_state s) -> const char* {
        switch (s) {
            case GRPC_CHANNEL_READY:              return "READY";
            case GRPC_CHANNEL_TRANSIENT_FAILURE:  return "FAILED";
            case GRPC_CHANNEL_CONNECTING:         return "CONNECTING";
            case GRPC_CHANNEL_IDLE:               return "IDLE";
            default:                               return "SHUTDOWN";
        }
    };

    // 错误计数从 Redis 读
    int err_spreadsheet = 0, err_file = 0, err_auth = 0;
    if (redis_ && redis_->IsConnected()) {
        err_spreadsheet = (int)redis_->GetInt("errors:spreadsheet:total");
        err_file = (int)redis_->GetInt("errors:file:total");
        err_auth = (int)redis_->GetInt("errors:auth:total");
    }

    response = std::string("{")
        + "\"services\":{"
        + "\"auth\":{\"channel\":\"" + state_str(auth_ch_ ? auth_ch_->GetState(true) : GRPC_CHANNEL_SHUTDOWN)
        + "\",\"breaker\":\"" + cb_auth_.StateStr() + "\"},"
        + "\"sheet\":{\"channel\":\"" + state_str(sheet_ch_ ? sheet_ch_->GetState(true) : GRPC_CHANNEL_SHUTDOWN)
        + "\",\"breaker\":\"" + cb_sheet_.StateStr() + "\"},"
        + "\"file\":{\"channel\":\"" + state_str(file_ch_ ? file_ch_->GetState(true) : GRPC_CHANNEL_SHUTDOWN)
        + "\",\"breaker\":\"" + cb_file_.StateStr() + "\"}},"
        + "\"errors\":{"
        + "\"auth\":" + std::to_string(err_auth) + ","
        + "\"spreadsheet\":" + std::to_string(err_spreadsheet) + ","
        + "\"file\":" + std::to_string(err_file) + "},"
        + "\"log_level\":\"active\""
        + "}";
    return true;
}

// Start
bool Gateway::Start() {
    // ---- 启动 CQ 协程调度线程 ----
    auth_cq_.Start();
    sheet_cq_.Start();
    file_cq_.Start();
    printf("[Gateway] CQ loops started (auth/sheet/file)\n");

    // ---- h2c server (8080) ----
    Http2Server svr;
    svr.set_pre_routing_handler([](const Http2Server::Request&, Http2Server::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, X-Idempotency-Key");
        res.set_header("Access-Control-Allow-Credentials", "true");
        return Http2Server::HandlerResponse::Unhandled;
    });
    svr.Options(R"(/api/.*)", [](const Http2Server::Request&, Http2Server::Response& res) { res.status = 204; });

    // ---- HTTP/1.1 server (8081, for nginx proxy) ----
    auto* http_svr = new httplib::Server();
    http_svr->set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, X-Idempotency-Key");
        res.set_header("Access-Control-Allow-Credentials", "true");
        return httplib::Server::HandlerResponse::Unhandled;
    });
    http_svr->Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    // ---- Shared helpers (Start scope, both servers) ----
    // require_auth_full: extracts both username (for logging) and user_id (for DB queries)
    auto require_auth_full = [this](const auto& req, auto& res, std::string& u, int64_t& uid, std::string& tok) -> bool {
        if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) {
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json"); res.status = 401; return false;
        }
        return true;
    };
    // Convenience overload that only needs username (auth-only routes)
    auto require_auth = [&require_auth_full](const auto& req, auto& res, std::string& u, std::string& tok) -> bool {
        int64_t uid = 0; return require_auth_full(req, res, u, uid, tok);
    };

    // Cookie属性: HttpOnly防JS读取; SameSite=Strict防CSRF; Path=/限制范围; 24h过期
    static const std::string kSetCookieFmt =
        "rpc_at=%s; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=900";
    static const std::string kClearCookie =
        "rpc_at=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0";

    auto login = [this, &kSetCookieFmt](auto& req, auto& res) {
        if (!cb_auth_.AllowRequest()) {
            res.set_content("{\"success\":false,\"error\":\"circuit open\"}", "application/json");
            return;
        }

        // 用户名级防爆破：跨 IP 累计失败 → 封锁（nginx 已处理单 IP 流速）
        std::string login_user = JsonGet(req.body, "username");
        if (redis_ && redis_->IsConnected() && !login_user.empty()) {
            if (redis_->GetInt("rate:block:" + login_user) >= 1) {
                res.status = 429;
                res.set_content(
                    "{\"success\":false,\"error\":\"Account temporarily locked. Retry later.\"}",
                    "application/json");
                return;
            }
            if (redis_->GetInt("rate:login:" + login_user + ":total") >= 15) {
                redis_->SetJSON("rate:block:" + login_user, "1", 1800);
                res.status = 429;
                res.set_content(
                    "{\"success\":false,\"error\":\"Account locked due to too many attempts.\"}",
                    "application/json");
                return;
            }
        }

        std::string r, at, rt;
        auto t0 = std::chrono::steady_clock::now();
        auto result = HandleLogin(req.body, r, at, rt);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (redis_ && redis_->IsConnected() && !login_user.empty()) {
            if (result == RpcResult::SUCCESS) {
                redis_->DeleteKey("rate:login:" + login_user + ":total");
            } else if (result == RpcResult::TRANSPORT_FAILURE || result == RpcResult::AUTH_FAILURE) {
                int64_t c = redis_->IncrementWithTTL("rate:login:" + login_user + ":total", 300);
                if (c >= 15) redis_->SetJSON("rate:block:" + login_user, "1", 1800);
            }
        }

        switch (result) {
            case RpcResult::SUCCESS:           cb_auth_.RecordResult(true, elapsed);  break;
            case RpcResult::TRANSPORT_FAILURE: cb_auth_.RecordResult(false, elapsed); break;
            case RpcResult::BUSINESS_FAILURE:  break;
            case RpcResult::AUTH_FAILURE:      break;
            case RpcResult::BAD_REQUEST:       res.status = 400; break;
        }
        if (!at.empty()) {
            char buf[2048]; snprintf(buf, sizeof(buf), kSetCookieFmt.c_str(), at.c_str());
            res.set_header("Set-Cookie", buf);
        }
        res.set_content(r, "application/json");
    };
    auto reg = [this](auto& req, auto& res) {
        if (!cb_auth_.AllowRequest()) {
            res.set_content("{\"success\":false,\"error\":\"circuit open\"}", "application/json");
            return;
        }

        // 用户名级防爆破（同 login）
        std::string reg_user = JsonGet(req.body, "username");
        if (redis_ && redis_->IsConnected() && !reg_user.empty()) {
            if (redis_->GetInt("rate:block:" + reg_user) >= 1) {
                res.status = 429;
                res.set_content(
                    "{\"success\":false,\"error\":\"Account temporarily locked. Retry later.\"}",
                    "application/json");
                return;
            }
            if (redis_->GetInt("rate:login:" + reg_user + ":total") >= 15) {
                redis_->SetJSON("rate:block:" + reg_user, "1", 1800);
                res.status = 429;
                res.set_content(
                    "{\"success\":false,\"error\":\"Account locked due to too many attempts.\"}",
                    "application/json");
                return;
            }
        }

        std::string r, at, rt;
        auto t0 = std::chrono::steady_clock::now();
        auto result = HandleRegister(req.body, r, at, rt);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (redis_ && redis_->IsConnected() && !reg_user.empty()) {
            if (result == RpcResult::SUCCESS) {
                redis_->DeleteKey("rate:login:" + reg_user + ":total");
            } else if (result == RpcResult::TRANSPORT_FAILURE || result == RpcResult::AUTH_FAILURE) {
                int64_t c = redis_->IncrementWithTTL("rate:login:" + reg_user + ":total", 300);
                if (c >= 15) redis_->SetJSON("rate:block:" + reg_user, "1", 1800);
            }
        }

        switch (result) {
            case RpcResult::SUCCESS:           cb_auth_.RecordResult(true, elapsed);  break;
            case RpcResult::TRANSPORT_FAILURE: cb_auth_.RecordResult(false, elapsed); break;
            case RpcResult::BUSINESS_FAILURE:  break;
            case RpcResult::AUTH_FAILURE:      break;
            case RpcResult::BAD_REQUEST:       res.status = 400; break;
        }
        if (!at.empty()) {
            char buf[2048]; snprintf(buf, sizeof(buf), kSetCookieFmt.c_str(), at.c_str());
            res.set_header("Set-Cookie", buf);
        }
        res.set_content(r, "application/json");
    };
    auto do_logout = [&kClearCookie](auto& /*req*/, auto& res) {
        res.set_header("Set-Cookie", kClearCookie);
        res.set_content("{\"success\":true}", "application/json");
    };
    auto refresh_tok = [this](auto& req, auto& res) {
        std::string rt = JsonGet(req.body, "refresh_token");
        std::string u  = JsonGet(req.body, "username");
        if (rt.empty() || u.empty()) { res.status = 400; res.set_content(R"({"error":"Missing fields"})", "application/json"); return; }
        std::string stored;
        if (!redis_ || !redis_->GetJSON("rt:" + u, stored) || stored != rt) {
            if (redis_) { redis_->DeleteKey("rt:" + u); redis_->DeleteKey("rate:login:" + u + ":total"); }
            res.status = 401; res.set_content(R"({"error":"Invalid refresh token"})", "application/json"); return;
        }
        std::string new_at = CreateAccessToken(u);
        char buf[2048]; snprintf(buf, sizeof(buf), kSetCookieFmt.c_str(), new_at.c_str());
        res.set_header("Set-Cookie", buf);
        res.set_content(R"({"success":true})", "application/json");
    };
    auto services    = [&require_auth, this](auto& req, auto& res) { std::string u, tok; if (!require_auth(req, res, u, tok)) return; std::string r; HandleServices(r); res.set_content(r, "application/json"); };
    auto health      = [this](auto&, auto& res) { std::string r; HandleHealth(r); res.set_content(r, "application/json"); };
    auto history     = [&require_auth, this](auto& req, auto& res) { std::string u, tok; if (!require_auth(req, res, u, tok)) return; std::string r; HandleHistory(r); res.set_content(r, "application/json"); };
    auto sys_status  = [&require_auth, this](auto& req, auto& res) { std::string u, tok; if (!require_auth(req, res, u, tok)) return; std::string r; HandleSystemStatus(r); res.set_content(r, "application/json"); };
    auto tx_begin    = [this, &require_auth](auto& req, auto& res) { std::string u, tok; if (!require_auth(req, res, u, tok)) return; std::string r; HandleTxBegin(u, req.body, r); res.set_content(r, "application/json"); };

    auto file_up = [this, &require_auth_full](auto& req, auto& res) {
        std::string u, tok; int64_t uid = 0;
        if (!require_auth_full(req, res, u, uid, tok)) return;
        if (!cb_file_.AllowRequest()) { res.set_content("{\"success\":false,\"error\":\"circuit open\"}", "application/json"); return; }
        auto file = req.form.get_file("file");
        if (file.filename.empty()) { res.set_content("{\"success\":false,\"error\":\"No file\"}", "application/json"); res.status = 400; return; }
        // 限制上传大小：gRPC 64MB 硬限制内留余量
        if (file.content.size() > 50 * 1024 * 1024) {
            res.set_content("{\"success\":false,\"error\":\"File too large, max 50MB\"}", "application/json");
            res.status = 413; return;
        }
        std::string mime = "application/octet-stream";
        auto dot = file.filename.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = file.filename.substr(dot);
            if (ext == ".png") mime = "image/png";
            else if (ext == ".jpg"||ext==".jpeg") mime = "image/jpeg";
            else if (ext == ".pdf") mime = "application/pdf";
            else if (ext == ".txt") mime = "text/plain";
            else if (ext == ".zip") mime = "application/zip";
            else if (ext == ".xlsx") mime = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
            else if (ext == ".mp4") mime = "video/mp4";
        }
        rpc::CreateFileRequest freq; rpc::CreateFileResponse fresp;
        freq.set_user_id(uid); freq.set_original_name(file.filename);
        freq.set_size(file.content.size()); freq.set_mime_type(mime);
        freq.set_file_content(file.content.data(), file.content.size());
        freq.set_idempotency_key(req.get_header_value("X-Idempotency-Key"));
        auto t0 = std::chrono::steady_clock::now();
        grpc::Status st; std::string peer;
        for (int attempt = 0; attempt < 2; ++attempt) {
            grpc::ClientContext ctx; ctx.AddMetadata("username", u);
            ctx.AddMetadata("authorization", "Bearer " + tok);
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
            st = file_stub_->CreateFile(&ctx, freq, &fresp);
            peer = ctx.peer();
            if (st.ok() && fresp.success()) break;
            if (!st.ok()) {
                rep_file_.RecordFailure(peer);
                if (rep_file_.AllowReplica(peer)) break; // not quarantined, give up
            } else break; // business failure, no retry
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!st.ok() || !fresp.success()) {
            if (!st.ok()) cb_file_.RecordResult(false, elapsed);
            res.set_content("{\"success\":false,\"error\":\"Upload failed\"}", "application/json"); return;
        }
        cb_file_.RecordResult(true, elapsed); rep_file_.RecordSuccess(peer);
        res.set_content("{\"success\":true,\"id\":" + std::to_string(fresp.id()) + "}", "application/json");
    };

    auto file_down = [this, &require_auth_full](auto& req, auto& res) {
        std::string u, tok; int64_t uid = 0;
        if (!require_auth_full(req, res, u, uid, tok)) return;
        std::string id_str = req.get_param_value("id");
        if (id_str.empty()) { res.set_content("{\"error\":\"Missing id\"}", "application/json"); res.status = 400; return; }
        if (!cb_file_.AllowRequest()) { res.set_content("{\"error\":\"circuit open\"}", "application/json"); return; }
        rpc::GetFileRequest freq; rpc::GetFileResponse fresp;
        freq.set_id(std::stoll(id_str)); freq.set_user_id(uid);
        auto t0 = std::chrono::steady_clock::now();
        grpc::Status st; std::string peer;
        for (int attempt = 0; attempt < 2; ++attempt) {
            grpc::ClientContext ctx; ctx.AddMetadata("username", u);
            ctx.AddMetadata("authorization", "Bearer " + tok);
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(600));
            st = file_stub_->GetFile(&ctx, freq, &fresp);
            peer = ctx.peer();
            if (st.ok() && fresp.success()) break;
            if (!st.ok()) {
                rep_file_.RecordFailure(peer);
                if (rep_file_.AllowReplica(peer)) break;
            } else break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!st.ok() || !fresp.success()) { cb_file_.RecordResult(false, elapsed); res.status = 404; return; }
        cb_file_.RecordResult(true, elapsed); rep_file_.RecordSuccess(peer);
        // Body comes from MySQL BLOB or MinIO via File service (no browser redirect to :443/:9000).
        res.set_header("Content-Disposition", "attachment; filename=\"" + fresp.file().original_name() + "\"");
        res.set_content(fresp.file_content(), fresp.file().mime_type());
    };

    // Auth+CB handler factory: auth check → inner(rpc_call) → CB success/fail (with latency)
    auto with_cb = [](CircuitBreaker& cb, auto inner) {
        return [&cb, inner = std::move(inner)](auto& req, auto& res) {
            std::string r;
            if (!cb.AllowRequest()) { r = "{\"success\":false,\"error\":\"circuit open\"}"; }
            else {
                auto t0 = std::chrono::steady_clock::now();
                auto result = inner(req, r);
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                switch (result) {
                    case RpcResult::SUCCESS:
                        cb.RecordResult(true, elapsed); break;
                    case RpcResult::TRANSPORT_FAILURE:
                        cb.RecordResult(false, elapsed); break;
                    case RpcResult::BUSINESS_FAILURE:  break;
                    case RpcResult::AUTH_FAILURE:      res.status = 401; break;
                    case RpcResult::BAD_REQUEST:       res.status = 400; break;
                }
            }
            res.set_content(r, "application/json");
        };
    };

    // Sheet inner handlers: auth check (Cookie) → rpc call → RpcResult
    auto sh_create  = [this](auto& req, std::string& r) {
        std::string u; int64_t uid = 0;
        std::string tok; if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) { r = "{\"error\":\"Unauthorized\"}"; return RpcResult::AUTH_FAILURE; }
        return HandleSheetCreate(u, uid, req.body, req.get_header_value("X-Idempotency-Key"), tok, rep_sheet_, r);
    };
    auto sh_list    = [this](auto& req, std::string& r) {
        std::string u; int64_t uid = 0;
        std::string tok; if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) { r = "{\"error\":\"Unauthorized\"}"; return RpcResult::AUTH_FAILURE; }
        std::string p = req.get_param_value("page"), ps = req.get_param_value("page_size");
        int page = p.empty() ? 0 : std::stoi(p);
        int page_size = ps.empty() ? 20 : std::stoi(ps);
        return HandleSheetList(u, uid, page, page_size, tok, rep_sheet_, r);
    };
    auto sh_get     = [this](auto& req, std::string& r) { std::string u; int64_t uid = 0; std::string tok; if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) { r = "{\"error\":\"Unauthorized\"}"; return RpcResult::AUTH_FAILURE; } return HandleSheetGet(u, uid, req.body, tok, rep_sheet_, r); };
    auto sh_update  = [this](auto& req, std::string& r) { std::string u; int64_t uid = 0; std::string tok; if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) { r = "{\"error\":\"Unauthorized\"}"; return RpcResult::AUTH_FAILURE; } return HandleSheetUpdate(u, uid, req.body, tok, rep_sheet_, r); };
    auto sh_delete  = [this](auto& req, std::string& r) { std::string u; int64_t uid = 0; std::string tok; if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) { r = "{\"error\":\"Unauthorized\"}"; return RpcResult::AUTH_FAILURE; } return HandleSheetDelete(u, uid, req.body, tok, rep_sheet_, r); };
    auto fl_list    = [this](auto& req, std::string& r) {
        std::string u; int64_t uid = 0;
        std::string tok; if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) { r = "{\"error\":\"Unauthorized\"}"; return RpcResult::AUTH_FAILURE; }
        std::string p = req.get_param_value("page"), ps = req.get_param_value("page_size");
        int page = p.empty() ? 0 : std::stoi(p);
        int page_size = ps.empty() ? 20 : std::stoi(ps);
        return HandleFileList(u, uid, page, page_size, tok, rep_file_, r);
    };
    auto fl_delete  = [this](auto& req, std::string& r) { std::string u; int64_t uid = 0; std::string tok; if (!VerifyAuth(req.get_header_value("Cookie"), u, uid, tok)) { r = "{\"error\":\"Unauthorized\"}"; return RpcResult::AUTH_FAILURE; } return HandleFileDelete(u, uid, req.body, tok, rep_file_, r); };

    // ---- Register on h2c server ----
    svr.Post("/api/login", login);
    svr.Post("/api/register", reg);
    svr.Post("/api/logout", do_logout);
    svr.Post("/api/refresh", refresh_tok);
    svr.Get("/api/services", services);
    svr.Post("/api/sheets", with_cb(cb_sheet_, sh_create));
    svr.Get("/api/sheets", with_cb(cb_sheet_, sh_list));
    svr.Post("/api/sheets/get", with_cb(cb_sheet_, sh_get));
    svr.Put("/api/sheets", with_cb(cb_sheet_, sh_update));
    svr.Post("/api/sheets/delete", with_cb(cb_sheet_, sh_delete));
    svr.Get("/api/files", with_cb(cb_file_, fl_list));
    svr.Post("/api/files/delete", with_cb(cb_file_, fl_delete));
    svr.Post("/api/files/upload", file_up);
    svr.Get("/api/files/download", file_down);
    svr.Post("/api/tx/begin", tx_begin);
    svr.Get("/api/health", health);
    svr.Get("/api/history", history);
    svr.Get("/api/system/status", sys_status);

    // ---- Register on HTTP/1.1 server (same handlers) ----
    http_svr->Post("/api/login", login);
    http_svr->Post("/api/register", reg);
    http_svr->Post("/api/logout", do_logout);
    http_svr->Post("/api/refresh", refresh_tok);
    http_svr->Get("/api/services", services);
    http_svr->Post("/api/sheets", with_cb(cb_sheet_, sh_create));
    http_svr->Get("/api/sheets", with_cb(cb_sheet_, sh_list));
    http_svr->Post("/api/sheets/get", with_cb(cb_sheet_, sh_get));
    http_svr->Put("/api/sheets", with_cb(cb_sheet_, sh_update));
    http_svr->Post("/api/sheets/delete", with_cb(cb_sheet_, sh_delete));
    http_svr->Get("/api/files", with_cb(cb_file_, fl_list));
    http_svr->Post("/api/files/delete", with_cb(cb_file_, fl_delete));
    http_svr->Post("/api/files/upload", file_up);
    http_svr->Get("/api/files/download", file_down);
    http_svr->Post("/api/tx/begin", tx_begin);
    http_svr->Get("/api/health", health);
    http_svr->Get("/api/history", history);
    http_svr->Get("/api/system/status", sys_status);

    if (!redis_cluster_seeds_.empty()) {
        redis_ = std::make_unique<RedisClient>(redis_cluster_seeds_, redis_password_, redis_pool_size_);
        if (redis_->Connect()) {
            // 将 Redis 注入熔断器，启用跨实例共享状态
            cb_auth_.SetRedis(redis_.get());
            cb_sheet_.SetRedis(redis_.get());
            cb_file_.SetRedis(redis_.get());
            rep_sheet_.SetRedis(redis_.get());
            rep_file_.SetRedis(redis_.get());
            printf("[Gateway] Circuit breakers connected to Redis (shared state enabled)\n");
        }
    }
    printf("[Gateway] h2c %s:%d  |  HTTP/1.1 8081\n", listen_addr_.c_str(), listen_port_);
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        WaitForBackends(5);
    }).detach();
    std::thread([http_svr]() {
        http_svr->listen("0.0.0.0", 8081);
        delete http_svr;
    }).detach();
    svr.listen(listen_addr_.c_str(), listen_port_);
    return true;
}

void Gateway::Stop() {}
