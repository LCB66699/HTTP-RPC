// Sheet Service main 
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "shared/base/rpc_interceptor.h"
#include "shared/base/call_logger.h"
#include "shared/client/database.h"
#include "shared/base/health_service_impl.h"
#include "shared/cache/l1_cache.h"
#include "shared/cache/l1_invalidator.h"
#include "shared/base/otel_tracer.h"
#include "shared/client/rabbit_publisher.h"
#include "shared/client/redis_client.h"
#include "shared/client/mongo_client.h"
#include "shared/base/snowflake.h"
#include "sheet/spreadsheet_service_impl.h"
#include "shared/base/system_logger.h"

static std::unique_ptr<grpc::Server> g_server;
static void SignalHandler(int) {
    if (g_server)
        g_server->Shutdown();
}

int main(int argc, char *argv[]) {
    std::string host = "0.0.0.0";
    int port = 50051;
    int mysql_shards = 2;
    std::string mysql_host = "mysql-spreadsheet", mysql_db = "rpc_spreadsheet";
    std::string mysql_user = "root", mysql_password = "123456";
    int mysql_port = 3306;
    int db_min_idle = 2;           // 写池最低保留连接数
    int db_idle_timeout_sec = 300;  // 空闲超时 5 分钟
    std::vector<std::string> redis_cluster_seeds;
    std::string redis_password;
    int redis_pool_size = 4;
    const char *env_secret = std::getenv("JWT_SECRET");
    if (!env_secret) { fprintf(stderr, "FATAL: JWT_SECRET environment variable is required\n"); return 1; }
    std::string jwt_secret = env_secret;

    #if HAS_OTEL
    InitTracer("spreadsheet-service");
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

    auto logger = std::make_unique<CallLogger>(1000, redis.get());
    const char *env_log = std::getenv("LOG_LEVEL");
    auto slog = std::make_unique<SystemLogger>("spreadsheet",
        env_log ? ParseLogLevel(env_log) : LogLevel::INFO);

    auto l1_cache = std::make_unique<L1Cache>(10000, 30);
    auto l1_invalidator = std::make_unique<L1CacheInvalidator>(l1_cache.get(), redis.get());
    l1_invalidator->Start();

    std::unique_ptr<RabbitPublisher> rabbit_pub;
    const char *rb_host = std::getenv("RABBITMQ_HOST");
    if (rb_host)
        rabbit_pub = std::make_unique<RabbitPublisher>(rb_host, 5672, "rpc", "rpc-rabbit-123456");

    minio::Client minio_client;
    const char *minio_ep = std::getenv("MINIO_ENDPOINT");
    const char *minio_ak = std::getenv("MINIO_ACCESS_KEY");
    const char *minio_sk = std::getenv("MINIO_SECRET_KEY");
    const char *minio_bk = std::getenv("MINIO_BUCKET");
    if (minio_ep && minio_ak && minio_sk && minio_bk) {
        minio_client.endpoint = minio_ep;
        minio_client.access_key = minio_ak;
        minio_client.secret_key = minio_sk;
        minio_client.bucket = minio_bk;
        const char *minio_pub = std::getenv("MINIO_PUBLIC_URL");
        minio_client.public_url = minio_pub ? minio_pub : ("http://" + std::string(minio_ep));
        printf("[Sheet] MinIO configured: %s/%s\n", minio_ep, minio_bk);
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    HealthMonitorImpl health_monitor;
    health_monitor.SetRedis(redis.get());
    health_monitor.SetNodeInfo("sheet-" + std::to_string(port), "sheet", host, port);
    health_monitor.StartHeartbeat();

    std::string addr = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);

    // gRPC interceptor — JWT authentication at transport level
    {
        std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> factories;
        factories.push_back(std::make_unique<RpcAuthInterceptorFactory>(jwt_secret));
        builder.experimental().SetInterceptorCreators(std::move(factories));
    }

    builder.RegisterService(&health_monitor);
    SpreadsheetServiceImpl sheet_service;
    const char *env_auth = std::getenv("AUTH_SVC_ADDR");
    std::string auth_addr = env_auth ? env_auth : "rpc-auth:50051";
    grpc::ChannelArguments args;
    args.SetLoadBalancingPolicyName("round_robin");
    sheet_service.SetAuthChannel(
        grpc::CreateCustomChannel("dns:///" + auth_addr, grpc::InsecureChannelCredentials(), args));
    sheet_service.SetDatabase(db.get());
    sheet_service.SetRedis(redis.get());
    sheet_service.SetL1Cache(l1_cache.get());
    sheet_service.SetLogger(logger.get());
    sheet_service.SetSysLog(slog.get());
    if (rabbit_pub)
        sheet_service.SetRabbitMQ(rabbit_pub.get());
    if (minio_client.IsConfigured())
        sheet_service.SetMinio(&minio_client);

#ifdef MONGOCXX_FOUND
    const char *env_mongo = std::getenv("MONGODB_URI");
    std::string mongo_uri = env_mongo ? env_mongo : "mongodb://mongodb:27017";
    auto mongo_client = std::make_unique<MongoClient>(mongo_uri, "rpc_sheets");
    if (mongo_client->Connect())
        sheet_service.SetMongo(mongo_client.get());
    else
        printf("[Sheet] WARNING: MongoDB not available, cells will not be persisted\n");
#else
    printf("[Sheet] MongoDB support not compiled in\n");
#endif

    builder.RegisterService(&sheet_service);

    g_server = builder.BuildAndStart();
    printf("[Sheet] Listening on %s\n", addr.c_str());
    g_server->Wait();
    return 0;
}
