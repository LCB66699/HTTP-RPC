// Search Service main — 最轻量，只依赖 httplib
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "health_service_impl.h"
#include "search_service_impl.h"

static std::unique_ptr<grpc::Server> g_server;
static void SignalHandler(int) {
    if (g_server)
        g_server->Shutdown();
}

int main(int argc, char *argv[]) {
    std::string host = "0.0.0.0";
    int port = 50051;
    const char *es_host = std::getenv("ES_HOST");

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            port = std::atoi(argv[++i]);
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::string addr = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);

    HealthMonitorImpl health_monitor;
    health_monitor.SetNodeInfo("search-" + std::to_string(port), "search", host, port);
    health_monitor.StartHeartbeat();
    builder.RegisterService(&health_monitor);

    SearchServiceImpl search_service(es_host ? es_host : "http://elasticsearch:9200");
    builder.RegisterService(&search_service);

    g_server = builder.BuildAndStart();
    printf("[Search] Listening on %s (ES: %s)\n", addr.c_str(), es_host ? es_host : "default");
    g_server->Wait();
    return 0;
}
