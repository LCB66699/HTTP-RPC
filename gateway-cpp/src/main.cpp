// gateway-cpp/main.cpp — HTTP Gateway + TM 启动入口
#include "gateway.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>

static Gateway* g_gw = nullptr;
static void SignalHandler(int sig) { if (g_gw) g_gw->Stop(); }

int main(int argc, char* argv[]) {
    std::string listen_addr = "0.0.0.0";
    int listen_port = 8080;
    std::string grpc_auth = "rpc-auth:50051";
    std::string grpc_sheet = "rpc-sheet:50051";
    std::string grpc_file = "rpc-file:50051";
    const char* env_secret = std::getenv("JWT_SECRET");
    if (!env_secret || std::strlen(env_secret) < 32) {
        fprintf(stderr, "[FATAL] JWT_SECRET not set or too short (< 32 chars). Refusing to start.\n");
        return 1;
    }
    std::string jwt_secret = env_secret;
    std::string web_dir = "./web-ui";
    std::string redis_host;
    std::string redis_slave_host;
    std::string redis_password;
    int redis_port = 6379;
    std::string redis_sentinel_host;
    int redis_sentinel_port = 26379;
    std::string redis_master_name = "redis-master";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i+1 < argc) listen_port = std::atoi(argv[++i]);
        else if (arg == "--listen" && i+1 < argc) listen_addr = argv[++i];
        else if (arg == "--grpc-auth" && i+1 < argc) grpc_auth = argv[++i];
        else if (arg == "--grpc-sheet" && i+1 < argc) grpc_sheet = argv[++i];
        else if (arg == "--grpc-file" && i+1 < argc) grpc_file = argv[++i];
        else if (arg == "--jwt-secret" && i+1 < argc) jwt_secret = argv[++i];
        else if (arg == "--web" && i+1 < argc) web_dir = argv[++i];
        else if (arg == "--redis-host" && i+1 < argc) redis_host = argv[++i];
        else if (arg == "--redis-master-host" && i+1 < argc) redis_host = argv[++i];
        else if (arg == "--redis-slave-host" && i+1 < argc) redis_slave_host = argv[++i];
        else if (arg == "--redis-password" && i+1 < argc) redis_password = argv[++i];
        else if (arg == "--redis-port" && i+1 < argc) redis_port = std::atoi(argv[++i]);
        else if (arg == "--redis-sentinel-host" && i+1 < argc) redis_sentinel_host = argv[++i];
        else if (arg == "--redis-sentinel-port" && i+1 < argc) redis_sentinel_port = std::atoi(argv[++i]);
        else if (arg == "--redis-master-name" && i+1 < argc) redis_master_name = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --port <port>              HTTP port (default: 8080)\n");
            printf("  --grpc-auth <addr>         AuthService addr\n");
            printf("  --grpc-sheet <addr>        SpreadsheetService addr\n");
            printf("  --grpc-file <addr>         FileService addr\n");
            printf("  --grpc-pool <n>            Channel pool size (default: 1)\n");
            printf("  --redis-master-host <host> Redis master (write)\n");
            printf("  --redis-slave-host <host>  Redis slave DNS alias (read)\n");
            printf("  --redis-password <pw>      Redis AUTH password\n");
            printf("  --redis-port <port>        Redis port (default: 6379)\n");
            printf("  --redis-sentinel-host <h>  Sentinel host (default: redis-sentinel)\n");
            printf("  --redis-sentinel-port <p>  Sentinel port (default: 26379)\n");
            printf("  --redis-master-name <n>    Sentinel master name (default: redis-master)\n");
            return 0;
        }
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    Gateway gw(listen_addr, listen_port, grpc_auth, grpc_sheet, grpc_file,
               jwt_secret, web_dir,
               redis_host, redis_slave_host, redis_password, redis_port,
               redis_sentinel_host, redis_sentinel_port, redis_master_name);
    g_gw = &gw;
    gw.Start();
    return 0;
}
