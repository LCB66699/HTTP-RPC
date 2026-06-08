// File Service main — 独立编译
#include "file_service_impl.h"
#include "auth_interceptor.h"
#include "database.h"
#include "redis_client.h"
#include "snowflake.h"
#include "l1_cache.h"
#include "rabbit_publisher.h"
#include "call_logger.h"
#include "system_logger.h"
#include "health_service_impl.h"
#include "tx_resource.h"
#include <grpcpp/grpcpp.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>

static std::unique_ptr<grpc::Server> g_server;
static void SignalHandler(int) { if (g_server) g_server->Shutdown(); }

int main(int argc, char* argv[]) {
    std::string host="0.0.0.0"; int port=50051; int mysql_shards=2;
    std::string mysql_host="mysql-file", mysql_db="rpc_file";
    std::string mysql_user="root", mysql_password="123456"; int mysql_port=3306;
    std::vector<std::string> redis_cluster_seeds;
    std::string redis_password; int redis_pool_size=4;
    const char* env_secret = std::getenv("JWT_SECRET");
    std::string jwt_secret = env_secret ? env_secret : "default-secret";

    for(int i=1;i<argc;++i){
        std::string arg=argv[i];
        if(arg=="--port"&&i+1<argc) port=std::atoi(argv[++i]);
        else if(arg=="--mysql-write-host"&&i+1<argc) mysql_host=argv[++i];
        else if(arg=="--mysql-password"&&i+1<argc) mysql_password=argv[++i];
        else if(arg=="--mysql-db"&&i+1<argc) mysql_db=argv[++i];
        else if(arg=="--mysql-shards"&&i+1<argc) mysql_shards=std::atoi(argv[++i]);
        else if(arg=="--redis-cluster"&&i+1<argc) redis_cluster_seeds.push_back(argv[++i]);
        else if(arg=="--redis-password"&&i+1<argc) redis_password=argv[++i];
    }

    auto db = std::make_unique<ShardedDatabase>(mysql_shards,mysql_host,mysql_port,mysql_host,mysql_port,mysql_user,mysql_password,mysql_db,4);
    db->InitializeAll(); db->StartHealthCheck();

    std::unique_ptr<RedisClient> redis;
    if(!redis_cluster_seeds.empty()){ redis=std::make_unique<RedisClient>(redis_cluster_seeds,redis_password,redis_pool_size); redis->Connect(); }

    int wid = std::hash<std::string>{}(host+std::to_string(port)) & 0x1F;
    Snowflake snowflake(wid); db->SetSnowflake(&snowflake);

    auto logger = std::make_unique<CallLogger>(1000,redis.get());
    auto slog = std::make_unique<SystemLogger>("file",LogLevel::INFO);

    auto l1_cache = std::make_unique<L1Cache>(10000,30);

    std::unique_ptr<RabbitPublisher> rabbit_pub;
    const char* rb_host = std::getenv("RABBITMQ_HOST");
    if(rb_host) rabbit_pub = std::make_unique<RabbitPublisher>(rb_host,5672,"rpc","rpc-rabbit-123456");

    signal(SIGINT,SignalHandler); signal(SIGTERM,SignalHandler);

    HealthMonitorImpl health_monitor;
    health_monitor.SetRedis(redis.get());
    health_monitor.SetNodeInfo("file-"+std::to_string(port),"file",host,port);
    health_monitor.StartHeartbeat();

    TxResource tx_resource; tx_resource.SetDatabase(db.get());

    std::string addr=host+":"+std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr,grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(64*1024*1024);
    builder.SetMaxReceiveMessageSize(64*1024*1024);

    builder.RegisterService(&health_monitor);
    FileServiceImpl file_service;
    AuthInterceptor auth_interceptor(jwt_secret);
    file_service.SetAuthInterceptor(&auth_interceptor);
    file_service.SetDatabase(db.get()); file_service.SetRedis(redis.get());
    file_service.SetL1Cache(l1_cache.get()); file_service.SetLogger(logger.get());
    file_service.SetSysLog(slog.get());
    if(rabbit_pub) file_service.SetRabbitMQ(rabbit_pub.get());
    builder.RegisterService(&file_service);
    builder.RegisterService(&tx_resource);

    g_server = builder.BuildAndStart();
    printf("[File] Listening on %s\n",addr.c_str());
    g_server->Wait();
    return 0;
}
