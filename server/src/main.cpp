// server/main.cpp — gRPC 服务端启动入口 (HTTP/2 + Protobuf)
// --service 参数决定启动哪些 Service（分布式部署）
#include "tx_resource.h"
#include "spreadsheet_service_impl.h"
#include "file_service_impl.h"
#include "auth_service_impl.h"
#include "rabbit_publisher.h"
#include "auth_interceptor.h"
#include "call_logger.h"
#include "health_service_impl.h"
#include "search_service_impl.h"
#include "database.h"
#include "redis_client.h"
#include "l1_cache.h"
#include "l1_invalidator.h"
#include "system_logger.h"
#include "minio_client.h"
#include "snowflake.h"
#include <grpcpp/health_check_service_interface.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <memory>

static std::unique_ptr<grpc::Server> g_server;

static void SignalHandler(int sig) {
    printf("\n[main] Received signal %d, shutting down...\n", sig);
    if (g_server) g_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
}

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    int port = 50051;
    std::string host = "0.0.0.0";
    const char* env_secret = std::getenv("JWT_SECRET");
    if (!env_secret || std::strlen(env_secret) < 32) {
        fprintf(stderr, "[FATAL] JWT_SECRET not set or too short (< 32 chars). Refusing to start.\n");
        return 1;
    }
    std::string jwt_secret = env_secret;
    std::string service = "all";  // all | auth | spreadsheet | file
    std::string mysql_host;      // backward compat: --mysql-host
    std::string mysql_read_hosts; // --mysql-read-hosts (comma-sep slaves)
    int mysql_port = 3306;
    std::string mysql_user = "root";
    std::string mysql_password;
    std::string mysql_db = "rpc_demo";
    int mysql_shards = 1;
    std::vector<std::string> redis_cluster_seeds;
    std::string redis_password;     // --redis-password
    int redis_pool_size = 4;
    std::string log_level = "info";          // --log-level: off|error|warn|info|debug

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--service" && i + 1 < argc) service = argv[++i];
        else if (arg == "--jwt-secret" && i + 1 < argc) jwt_secret = argv[++i];
        else if (arg == "--mysql-host" && i + 1 < argc) mysql_host = argv[++i];
        else if (arg == "--mysql-port" && i + 1 < argc) mysql_port = std::atoi(argv[++i]);
        else if (arg == "--mysql-user" && i + 1 < argc) mysql_user = argv[++i];
        else if (arg == "--mysql-password" && i + 1 < argc) mysql_password = argv[++i];
        else if (arg == "--mysql-db" && i + 1 < argc) mysql_db = argv[++i];
        else if (arg == "--mysql-shards" && i + 1 < argc) mysql_shards = std::atoi(argv[++i]);
        else if (arg == "--redis-cluster" && i + 1 < argc) {
            redis_cluster_seeds.push_back(argv[++i]);
        }
        else if (arg == "--redis-password" && i + 1 < argc) redis_password = argv[++i];
        else if (arg == "--redis-pool-size" && i + 1 < argc) redis_pool_size = std::atoi(argv[++i]);
        else if (arg == "--log-level" && i + 1 < argc) log_level = argv[++i];
        else if (arg == "--mysql-write-host" && i + 1 < argc) mysql_host = argv[++i];
        else if (arg == "--mysql-read-hosts" && i + 1 < argc) mysql_read_hosts = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --port <port>              gRPC listen port (default: 50051)\n");
            printf("  --service <name>           Service: all|auth|spreadsheet|file (default: all)\n");
            printf("  --mysql-write-host <host>  MySQL master (write)\n");
            printf("  --mysql-read-hosts <h1,h2> MySQL slaves (read, comma-sep)\n");
            printf("  --mysql-port <port>        MySQL port (default: 3306)\n");
            printf("  --mysql-user <user>        MySQL user (default: root)\n");
            printf("  --mysql-password <pw>      MySQL password\n");
            printf("  --mysql-db <db>            MySQL database name\n");
            printf("  --mysql-shards <n>         Number of DB shards (default: 1)\n");
            printf("  --redis-cluster <h:p>      Add Redis Cluster seed node (can repeat)\n");
            printf("  --redis-password <pw>      Redis AUTH password\n");
            printf("  --redis-pool-size <n>      Connections per seed (default: 4)\n");
            printf("  --log-level <level>        off|error|warn|info|debug (default: info)\n");
            return 0;
        }
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // ---- MinIO (optional, read from env vars set by docker-compose) ----
    // MINIO_ENDPOINT     = host:port inside Docker network, e.g. "minio:9000"
    // MINIO_PUBLIC_URL   = URL browsers use to reach MinIO, e.g. "http://localhost:9000"
    // MINIO_ACCESS_KEY   = MinIO root user / access key
    // MINIO_SECRET_KEY   = MinIO root password / secret key
    // MINIO_BUCKET       = target bucket name (created automatically if missing)
    std::unique_ptr<minio::Client> minio_client;
    {
        const char* ep  = std::getenv("MINIO_ENDPOINT");
        const char* pub = std::getenv("MINIO_PUBLIC_URL");
        const char* ak  = std::getenv("MINIO_ACCESS_KEY");
        const char* sk  = std::getenv("MINIO_SECRET_KEY");
        const char* bk  = std::getenv("MINIO_BUCKET");
        if (ep && ak && sk && bk) {
            minio_client = std::make_unique<minio::Client>();
            minio_client->endpoint   = ep;
            minio_client->public_url = pub ? pub : "";
            minio_client->access_key = ak;
            minio_client->secret_key = sk;
            minio_client->bucket     = bk;
            printf("[main] MinIO configured: endpoint=%s bucket=%s\n", ep, bk);
        } else {
            printf("[main] MinIO not configured — file uploads will be rejected\n");
        }
    }

    // ---- Database + Redis ----
    std::unique_ptr<ShardedDatabase> db;
    if (!mysql_host.empty()) {
        std::string read_hosts = mysql_read_hosts.empty() ? mysql_host : mysql_read_hosts;
        // When shards==1, ShardedDatabase creates exactly one underlying Database using
        // the host name as-is (no -0 suffix appended, no db name suffix applied).
        // When shards>1, each shard i gets host "{prefix}-{i}" and db "{prefix}_{i}".
        db = std::make_unique<ShardedDatabase>(mysql_shards,
                                                mysql_host, mysql_port,
                                                read_hosts, mysql_port,
                                                mysql_user, mysql_password,
                                                mysql_db, 4);
        db->InitializeAll();
        db->StartHealthCheck();
    }

    std::unique_ptr<RedisClient> redis;
    if (!redis_cluster_seeds.empty()) {
        redis = std::make_unique<RedisClient>(redis_cluster_seeds, redis_password, redis_pool_size);
        redis->Connect();
    }

    // Snowflake 动态 worker_id（Redis 已连接后执行）
    auto resolve_worker = [&]() -> int {
        const char* env_wid = std::getenv("SNOWFLAKE_WORKER_ID");
        if (env_wid) {
            int id = std::atoi(env_wid) & 0x1F;
            printf("[main] Snowflake worker_id=%d (from SNOWFLAKE_WORKER_ID)\n", id);
            return id;
        }
        const char* hn = std::getenv("HOSTNAME");
        std::string hostname = hn ? hn : ("svc-" + std::to_string(port));
        if (redis && redis->IsConnected()) {
            std::string reg_key = "snowflake:host:" + hostname;
            int64_t existing = redis->GetInt(reg_key);
            if (existing > 0) {
                int id = (int)(existing - 1) & 0x1F;
                printf("[main] Snowflake worker_id=%d (Redis restored for %s)\n", id, hostname.c_str());
                return id;
            }
            for (int i = 0; i < 32; ++i) {
                if (redis->SetNX("snowflake:slot:" + std::to_string(i), hostname, 3600)) {
                    redis->SetJSON(reg_key, std::to_string(i + 1), 86400);
                    printf("[main] Snowflake worker_id=%d (Redis registered %s)\n", i, hostname.c_str());
                    return i;
                }
            }
            printf("[main] Snowflake: all 32 slots occupied, falling back to hash\n");
        }
        int id = std::hash<std::string>{}(hostname) & 0x1F;
        printf("[main] Snowflake worker_id=%d (from hostname hash)\n", id);
        return id;
    };
    Snowflake snowflake(resolve_worker());
    if (db) db->SetSnowflake(&snowflake);

    auto logger = std::make_unique<CallLogger>(1000, redis.get());

    auto slog = std::make_unique<SystemLogger>(service, ParseLogLevel(log_level));
    slog->SetRedis(redis.get());
    printf("[main] Log level: %s\n", log_level.c_str());

    // L1 local cache + Pub/Sub invalidator
    auto l1_cache = std::make_unique<L1Cache>(10000, 30);
    auto l1_invalidator = std::make_unique<L1CacheInvalidator>(l1_cache.get(), redis.get());
    l1_invalidator->Start();
    printf("[main] L1 cache initialized (max=10000, ttl=30s)\n");

    // ---- gRPC Server Setup ----
    grpc::EnableDefaultHealthCheckService(true);
    std::string addr = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 30000);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);

    // 恢复线程
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // HealthMonitor（所有实例启用心跳）
    HealthMonitorImpl health_monitor;
    health_monitor.SetRedis(redis.get());
    health_monitor.SetNodeInfo(service + "-" + std::to_string(port), service, host, port);
    health_monitor.StartHeartbeat();
    builder.RegisterService(&health_monitor);

    // TxResource（所有 Service 都注册）
    TxResource tx_resource;
    tx_resource.SetDatabase(db.get());

    // ---- Register Services ----
    std::unique_ptr<AuthServiceImpl> auth_service;
    SpreadsheetServiceImpl spreadsheet_service;
    FileServiceImpl file_service;
    std::unique_ptr<AuthInterceptor> sheet_auth;
    std::unique_ptr<AuthInterceptor> file_auth;

    if (service == "all" || service == "auth") {
        auth_service = std::make_unique<AuthServiceImpl>(jwt_secret, db.get());
        auth_service->SetLogger(logger.get());
        auth_service->SetRedis(redis.get());
        auth_service->SetSysLog(slog.get());
        builder.RegisterService(auth_service.get());
        printf("[main] AuthService registered\n");
    }

    // 简单 JSON 解析辅助
    auto jget = [](const std::string& json, const std::string& key) -> std::string {
        std::string pat = "\"" + key + "\"";
        auto pos = json.find(pat);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + pat.size());
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size()) return "";
        if (json[pos] == '"') {
            auto start = pos + 1;
            auto end = json.find('"', start);
            while (end != std::string::npos && json[end-1] == '\\') end = json.find('"', end + 1);
            if (end == std::string::npos) return "";
            std::string val;
            for (size_t i = start; i < end; i++) {
                if (json[i] == '\\' && i + 1 < end) val += json[++i];
                else val += json[i];
            }
            return val;
        } else {
            auto end = pos;
            while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ' && json[end] != '\n') end++;
            return json.substr(pos, end - pos);
        }
    };

    // 创建到 Auth 服务的 gRPC 通道（供 Sheet/File 服务间调用）
    std::shared_ptr<grpc::Channel> auth_svc_ch;
    if (service != "auth") {
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 60000);
        args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
        auth_svc_ch = grpc::CreateCustomChannel(
            "rpc-auth:50051", grpc::InsecureChannelCredentials(), args);
    }

    std::unique_ptr<RabbitPublisher> rabbit_pub;
    const char* rb_host = std::getenv("RABBITMQ_HOST");
    if (rb_host) {
        rabbit_pub = std::make_unique<RabbitPublisher>(
            rb_host, 5672,
            std::getenv("RABBITMQ_USER") ? std::getenv("RABBITMQ_USER") : "rpc",
            std::getenv("RABBITMQ_PASS") ? std::getenv("RABBITMQ_PASS") : "rpc-rabbit-123456");
    }

    if (service == "all" || service == "spreadsheet") {
        sheet_auth = std::make_unique<AuthInterceptor>(jwt_secret);
        spreadsheet_service.SetAuthInterceptor(sheet_auth.get());
        if (auth_svc_ch) spreadsheet_service.SetAuthChannel(auth_svc_ch);
        if (rabbit_pub) spreadsheet_service.SetRabbitMQ(rabbit_pub.get());
        spreadsheet_service.SetDatabase(db.get());
        spreadsheet_service.SetRedis(redis.get());
        spreadsheet_service.SetL1Cache(l1_cache.get());
        spreadsheet_service.SetLogger(logger.get());
        spreadsheet_service.SetSysLog(slog.get());

        // CreateSheet: 插入表格 + 写 undo_log
        tx_resource.RegisterHandler("CreateSheet", [&db, jget](const std::string& params, std::string& err) -> bool {
            std::string username = jget(params, "username");
            std::string name = jget(params, "name");
            std::string desc = jget(params, "description");
            std::string headers = jget(params, "headers_json");
            std::string data = jget(params, "data_json");
            if (username.empty() || name.empty()) { err = "Missing params"; return false; }
            int64_t user_id = db->GetUserId(username);
            if (user_id < 0) { err = "User not found"; return false; }
            int64_t id = 0;
            if (!db->CreateSpreadsheet(user_id, username, name, desc, headers, data, id)) { err = "DB error"; return false; }
            db->WriteUndoLog(jget(params, "xid"), "spreadsheets", id, "{}");
            return true;
        });

        // DeleteSheet: 先存快照 → 删表 → 写 undo_log
        tx_resource.RegisterHandler("DeleteSheet", [&db, jget](const std::string& params, std::string& err) -> bool {
            std::string id_str = jget(params, "id");
            int64_t id = id_str.empty() ? 0 : std::stoll(id_str);
            if (id <= 0) { err = "Missing id"; return false; }
            SpreadsheetRow row;
            if (!db->GetSpreadsheet(id, 0, row)) { err = "Not found"; return false; }
            std::string snapshot = "{\"name\":\"" + row.name + "\",\"description\":\"" + row.description
                + "\",\"headers\":" + row.headers_json + ",\"data\":" + row.data_json
                + ",\"row_count\":" + std::to_string(row.row_count)
                + ",\"col_count\":" + std::to_string(row.col_count) + "}";
            if (!db->DeleteSpreadsheet(id)) { err = "DB error"; return false; }
            db->WriteUndoLog(jget(params, "xid"), "spreadsheets", id, snapshot);
            return true;
        });

        builder.RegisterService(&spreadsheet_service);
        builder.RegisterService(&tx_resource);
        printf("[main] SpreadsheetService + TxResource registered\n");
    }

    if (service == "all" || service == "file") {
        file_auth = std::make_unique<AuthInterceptor>(jwt_secret);
        file_service.SetAuthInterceptor(file_auth.get());
        if (auth_svc_ch) file_service.SetAuthChannel(auth_svc_ch);
        if (rabbit_pub) file_service.SetRabbitMQ(rabbit_pub.get());
        file_service.SetDatabase(db.get());
        file_service.SetRedis(redis.get());
        file_service.SetL1Cache(l1_cache.get());
        file_service.SetLogger(logger.get());
        file_service.SetSysLog(slog.get());
        if (minio_client) file_service.SetMinio(minio_client.get());

        // CreateFile: 插入文件元数据占位 + 写 undo_log（2PC 路径不含文件内容）
        tx_resource.RegisterHandler("CreateFile", [&db, jget](const std::string& params, std::string& err) -> bool {
            std::string username = jget(params, "username");
            std::string origin = jget(params, "original_name");
            int64_t size = std::stoll(jget(params, "size"));
            std::string mime = jget(params, "mime_type");
            if (username.empty() || origin.empty()) { err = "Missing params"; return false; }
            int64_t user_id = db->GetUserId(username);
            if (user_id < 0) { err = "User not found"; return false; }
            int64_t id = 0;
            if (!db->CreateFile(user_id, username, origin, size, mime, std::string(), id)) { err = "DB error"; return false; }
            db->WriteUndoLog(jget(params, "xid"), "files", id, "{}");
            return true;
        });
        // DeleteFile: 先存快照 → 删文件 → 写 undo_log
        tx_resource.RegisterHandler("DeleteFile", [&db, jget, &minio_client](const std::string& params, std::string& err) -> bool {
            std::string id_str = jget(params, "id");
            int64_t id = id_str.empty() ? 0 : std::stoll(id_str);
            if (id <= 0) { err = "Missing id"; return false; }
            FileRow row;
            if (!db->GetFile(id, 0, row)) { err = "Not found"; return false; }
            // Delete MinIO object before removing the MySQL row
            if (!row.storage_path.empty() && minio_client && minio_client->IsConfigured()) {
                if (!minio_client->DeleteObject(row.storage_path)) {
                    fprintf(stderr, "[2PC] DeleteFile MinIO DeleteObject failed for key=%s\n", row.storage_path.c_str());
                }
            }
            std::string snapshot = "{\"original_name\":\"" + row.original_name
                + "\",\"size\":" + std::to_string(row.size)
                + ",\"mime_type\":\"" + row.mime_type + "\"}";
            if (!db->DeleteFile(id)) { err = "DB error"; return false; }
            db->WriteUndoLog(jget(params, "xid"), "files", id, snapshot);
            return true;
        });

        builder.RegisterService(&file_service);
        builder.RegisterService(&tx_resource);
        printf("[main] FileService + TxResource registered\n");
    }

    if (service == "all" || service == "search") {
        const char* es_host = std::getenv("ES_HOST");
        SearchServiceImpl search_service(es_host ? es_host : "http://elasticsearch:9200");
        builder.RegisterService(&search_service);
        printf("[main] SearchService registered (ES: %s)\n", es_host ? es_host : "default");
    }

    g_server = builder.BuildAndStart();
    if (!g_server) {
        fprintf(stderr, "Failed to start gRPC server on %s\n", addr.c_str());
        return 1;
    }

    printf("========================================\n");
    printf("  Service: %s\n", service.c_str());
    printf("  gRPC:   %s\n", addr.c_str());
    if (!mysql_host.empty()) printf("  MySQL:  %s:%d/%s (shards=%d)\n", mysql_host.c_str(), mysql_port, mysql_db.c_str(), mysql_shards);
    if (!redis_cluster_seeds.empty()) printf("  Redis Cluster: %zu seeds\n", redis_cluster_seeds.size());
    printf("========================================\n");

    g_server->Wait();
    return 0;
}
