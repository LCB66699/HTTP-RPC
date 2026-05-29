// gateway-cpp/main.cpp — HTTP Gateway + TM 启动入口
#include "gateway.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>

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
    std::vector<std::string> redis_cluster_seeds;
    std::string redis_password;
    int redis_pool_size = 4;
    int max_concurrent = 256;
    int queue_timeout_ms = 500;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i+1 < argc) listen_port = std::atoi(argv[++i]);
        else if (arg == "--listen" && i+1 < argc) listen_addr = argv[++i];
        else if (arg == "--grpc-auth" && i+1 < argc) grpc_auth = argv[++i];
        else if (arg == "--grpc-sheet" && i+1 < argc) grpc_sheet = argv[++i];
        else if (arg == "--grpc-file" && i+1 < argc) grpc_file = argv[++i];
        else if (arg == "--jwt-secret" && i+1 < argc) jwt_secret = argv[++i];
        else if (arg == "--web" && i+1 < argc) web_dir = argv[++i];
        else if (arg == "--redis-cluster" && i+1 < argc) {
            redis_cluster_seeds.push_back(argv[++i]);
        }
        else if (arg == "--redis-password" && i+1 < argc) redis_password = argv[++i];
        else if (arg == "--redis-pool-size" && i+1 < argc) redis_pool_size = std::atoi(argv[++i]);
        else if (arg == "--max-concurrent" && i+1 < argc) max_concurrent = std::atoi(argv[++i]);
        else if (arg == "--queue-timeout-ms" && i+1 < argc) queue_timeout_ms = std::atoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --port <port>              HTTP port (default: 8080)\n");
            printf("  --grpc-auth <addr>         AuthService addr\n");
            printf("  --grpc-sheet <addr>        SpreadsheetService addr\n");
            printf("  --grpc-file <addr>         FileService addr\n");
            printf("  --redis-cluster <h:p>      Add Redis Cluster seed node (can repeat)\n");
            printf("  --redis-password <pw>      Redis AUTH password\n");
            printf("  --redis-pool-size <n>      Connections per seed node (default: 4)\n");
            printf("  --max-concurrent <n>       Max concurrent requests (default: 256)\n");
            printf("  --queue-timeout-ms <ms>    Queue wait timeout (default: 3000)\n");
            return 0;
        }
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    Gateway gw(listen_addr, listen_port, grpc_auth, grpc_sheet, grpc_file,
               jwt_secret, web_dir,
               redis_cluster_seeds, redis_password, redis_pool_size,
               max_concurrent, queue_timeout_ms);
    g_gw = &gw;
    gw.Start();
    return 0;
}
