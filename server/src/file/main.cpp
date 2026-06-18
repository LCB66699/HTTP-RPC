// File Service main 鈥?鐙珛缂栬瘧
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "auth_interceptor.h"
#include "call_logger.h"
#include "database.h"
#include "file_service_impl.h"
#include "health_service_impl.h"
#include "l1_cache.h"
#include "otel_tracer.h"
#include "rabbit_publisher.h"
#include "redis_client.h"
#include "snowflake.h"
#include "system_logger.h"

static std::unique_ptr<grpc::Server> g_server;
static void SignalHandler(int) {
    if (g_server)
        g_server->Shutdown();
}

int main(int argc, char *argv[]) {
    std::string host = "0.0.0.0";
    int port = 50051;
    int mysql_shards = 2;
    std::string mysql_host = "mysql-file", mysql_db = "rpc_file";
    std::string mysql_user = "root", mysql_password = "123456";
    int mysql_port = 3306;
    std::vector<std::string> redis_cluster_seeds;
    std::string redis_password;
    int redis_pool_size = 4;
    const char *env_secret = std::getenv("JWT_SECRET");
    std::string jwt_secret = env_secret ? env_secret : "default-secret";

    #if HAS_OTEL
    InitTracer("file-service");
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
    }

    auto db = std::make_unique<ShardedDatabase>(mysql_shards, mysql_host, mysql_port, mysql_host, mysql_port,
                                                mysql_user, mysql_password, mysql_db, 4);
    db->InitializeAll();
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
    auto slog = std::make_unique<SystemLogger>("file",
        env_log ? ParseLogLevel(env_log) : LogLevel::INFO);

    auto l1_cache = std::make_unique<L1Cache>(10000, 30);

    std::unique_ptr<RabbitPublisher> rabbit_pub;
    const char *rb_host = std::getenv("RABBITMQ_HOST");
    if (rb_host)
        rabbit_pub = std::make_unique<RabbitPublisher>(rb_host, 5672, "rpc", "rpc-rabbit-123456");

    // MinIO client
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
        printf("[File] MinIO configured: %s/%s\n", minio_ep, minio_bk);
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    HealthMonitorImpl health_monitor;
    health_monitor.SetRedis(redis.get());
    health_monitor.SetNodeInfo("file-" + std::to_string(port), "file", host, port);
    health_monitor.StartHeartbeat();

    std::string addr = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);

    builder.RegisterService(&health_monitor);
    FileServiceImpl file_service;
    AuthInterceptor auth_interceptor(jwt_secret);
    file_service.SetAuthInterceptor(&auth_interceptor);
    const char *env_auth = std::getenv("AUTH_SVC_ADDR");
    std::string auth_addr = env_auth ? env_auth : "rpc-auth:50051";
    grpc::ChannelArguments args;
    args.SetLoadBalancingPolicyName("round_robin");
    file_service.SetAuthChannel(
        grpc::CreateCustomChannel("dns:///" + auth_addr, grpc::InsecureChannelCredentials(), args));
    file_service.SetDatabase(db.get());
    file_service.SetRedis(redis.get());
    file_service.SetL1Cache(l1_cache.get());
    file_service.SetLogger(logger.get());
    file_service.SetSysLog(slog.get());
    if (rabbit_pub)
        file_service.SetRabbitMQ(rabbit_pub.get());
    if (minio_client.IsConfigured())
        file_service.SetMinio(&minio_client);
    builder.RegisterService(&file_service);

    g_server = builder.BuildAndStart();
    printf("[File] Listening on %s\n", addr.c_str());
    g_server->Wait();
    return 0;
}
