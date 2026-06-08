// Auth Service 入口
#include <grpcpp/grpcpp.h>
#include "generated/rpc_auth.grpc.pb.h"
#include "auth_service_impl.h"
#include "database.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[]) {
    std::string listen_addr = "0.0.0.0:50051";
    std::string jwt_secret;
    std::string mysql_write, mysql_reads, mysql_password, mysql_db;
    int mysql_shards = 1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i+1 < argc) listen_addr = std::string("0.0.0.0:") + argv[++i];
        else if (arg == "--jwt-secret" && i+1 < argc) jwt_secret = argv[++i];
        else if (arg == "--mysql-write-host" && i+1 < argc) mysql_write = argv[++i];
        else if (arg == "--mysql-read-hosts" && i+1 < argc) mysql_reads = argv[++i];
        else if (arg == "--mysql-password" && i+1 < argc) mysql_password = argv[++i];
        else if (arg == "--mysql-db" && i+1 < argc) mysql_db = argv[++i];
        else if (arg == "--mysql-shards" && i+1 < argc) mysql_shards = std::atoi(argv[++i]);
    }

    const char* env_secret = std::getenv("JWT_SECRET");
    if (jwt_secret.empty()) jwt_secret = env_secret ? env_secret : "default-secret";

    ShardedDatabase db(mysql_write, mysql_reads, mysql_password, mysql_db, mysql_shards, nullptr);

    AuthServiceImpl service(jwt_secret, &db);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    printf("[Auth] Listening on %s\n", listen_addr.c_str());
    server->Wait();
    return 0;
}
