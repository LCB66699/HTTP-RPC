// Search Service main 鈥?鏈€杞婚噺锛屽彧渚濊禆 httplib
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "shared/base/health_service_impl.h"
#include "shared/base/otel_tracer.h"
#include "shared/base/rpc_interceptor.h"
#include "search/search_service_impl.h"

static std::unique_ptr<grpc::Server> g_server;
static void SignalHandler(int) {
    if (g_server)
        g_server->Shutdown();
}

int main(int argc, char *argv[]) {
    std::string host = "0.0.0.0";
    int port = 50051;
    const char *es_host = std::getenv("ES_HOST");
    const char *env_secret = std::getenv("JWT_SECRET");
    std::string jwt_secret = env_secret ? env_secret : "default-secret";

    #if HAS_OTEL
    InitTracer("search-service");
#endif

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

    // gRPC interceptor — JWT authentication at transport level
    {
        std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> factories;
        factories.push_back(std::make_unique<RpcAuthInterceptorFactory>(jwt_secret));
        builder.experimental().SetInterceptorCreators(std::move(factories));
    }

    SearchServiceImpl search_service(es_host ? es_host : "http://elasticsearch:9200");
    builder.RegisterService(&search_service);

    g_server = builder.BuildAndStart();
    printf("[Search] Listening on %s (ES: %s)\n", addr.c_str(), es_host ? es_host : "default");
    g_server->Wait();
    return 0;
}
