// Auth Service main 鈥?鐙珛缂栬瘧锛屼笉寮曠敤 Sheet/File/Search
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "shared/base/auth_interceptor.h"
#include "auth/auth_service_impl.h"
#include "auth/sharing_service_impl.h"
#include "shared/base/call_logger.h"
#include "shared/client/database.h"
#include "shared/base/health_service_impl.h"
#include "shared/base/otel_tracer.h"
#include "shared/client/redis_client.h"
#include "shared/base/snowflake.h"
#include "shared/base/system_logger.h"

static std::unique_ptr<grpc::Server> g_server;
static void SignalHandler(int) {
    if (g_server)
        g_server->Shutdown();
}

int main(int argc, char *argv[]) {
    std::string host = "0.0.0.0";
    int port = 50051;
    int mysql_shards = 1;
    std::string mysql_host = "mysql-auth", mysql_db = "rpc_auth";
    std::string mysql_user = "root", mysql_password = "123456";
    int mysql_port = 3306;
    int db_min_idle = 2;
    int db_idle_timeout_sec = 300;
    std::vector<std::string> redis_cluster_seeds;
    std::string redis_password;
    int redis_pool_size = 4;
    const char *env_secret = std::getenv("JWT_SECRET");
    if (!env_secret) { fprintf(stderr, "FATAL: JWT_SECRET environment variable is required\n"); return 1; }
    std::string jwt_secret = env_secret;

    #if HAS_OTEL
    InitTracer("auth-service");
#endif

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            port = std::atoi(argv[++i]);
        else if (arg == "--mysql-write-host" && i + 1 < argc)
            mysql_host = argv[++i];
        else if (arg == "--mysql-password" && i + 1 < argc)
            mysql_password = argv[++i];
        else if (arg == "--mysql-db" && i + 1 < argc)
            mysql_db = argv[++i];
        else if (arg == "--mysql-shards" && i + 1 < argc)
            mysql_shards = std::atoi(argv[++i]);
        else if (arg == "--mysql-port" && i + 1 < argc)
            mysql_port = std::atoi(argv[++i]);
        else if (arg == "--redis-cluster" && i + 1 < argc)
            redis_cluster_seeds.push_back(argv[++i]);
        else if (arg == "--redis-password" && i + 1 < argc)
            redis_password = argv[++i];
        else if (arg == "--db-min-idle" && i + 1 < argc)
            db_min_idle = std::atoi(argv[++i]);
        else if (arg == "--db-idle-timeout-sec" && i + 1 < argc)
            db_idle_timeout_sec = std::atoi(argv[++i]);
    }

    auto db = std::make_unique<ShardedDatabase>(mysql_shards, mysql_host, mysql_port, mysql_host, mysql_port,
                                                mysql_user, mysql_password, mysql_db, 4);
    db->InitializeAll();
    db->SetMinIdle(db_min_idle);
    db->SetIdleTimeoutSec(db_idle_timeout_sec);
    db->StartHealthCheck();

    std::unique_ptr<RedisClient> redis;
    if (!redis_cluster_seeds.empty()) {
        redis = std::make_unique<RedisClient>(redis_cluster_seeds, redis_password, redis_pool_size);
        redis->Connect();
    }

    int wid = std::hash<std::string>{}(host + std::to_string(port)) & 0x1F;
    Snowflake snowflake(wid);
    db->SetSnowflake(&snowflake);
    printf("[main] Snowflake worker_id=%d\n", wid);

    auto logger = std::make_unique<CallLogger>(1000, redis.get());
    const char *env_log = std::getenv("LOG_LEVEL");
    auto slog = std::make_unique<SystemLogger>("auth",
        env_log ? ParseLogLevel(env_log) : LogLevel::INFO);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    HealthMonitorImpl health_monitor;
    health_monitor.SetRedis(redis.get());
    health_monitor.SetNodeInfo("auth-" + std::to_string(port), "auth", host, port);
    health_monitor.StartHeartbeat();

    std::string addr = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);

    builder.RegisterService(&health_monitor);
    AuthServiceImpl auth_service(jwt_secret, db.get());
    auth_service.SetLogger(logger.get());
    auth_service.SetRedis(redis.get());
    auth_service.SetSysLog(slog.get());
    builder.RegisterService(&auth_service);

    SharingServiceImpl sharing_service(db.get());
    builder.RegisterService(&sharing_service);

    grpc::EnableDefaultHealthCheckService(true);
    g_server = builder.BuildAndStart();
    printf("[Auth] Listening on %s\n", addr.c_str());
    g_server->Wait();
    return 0;
}
