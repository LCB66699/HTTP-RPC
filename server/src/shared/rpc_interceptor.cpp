#include "rpc_interceptor.h"

thread_local AuthContext g_rpc_auth_ctx{};

RpcAuthInterceptor::RpcAuthInterceptor(const std::string &jwt_secret) : jwt_secret_(jwt_secret) {}

void RpcAuthInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods *methods) {
    // POST_RECV_INITIAL_METADATA fires after the server receives client headers.
    // This is the only hook where we can access client metadata in gRPC 1.51.
    if (methods->QueryInterceptionHookPoint(
            grpc::experimental::InterceptionHookPoints::POST_RECV_INITIAL_METADATA)) {
        auto *metadata = methods->GetRecvInitialMetadata();
        bool found = false;
        if (metadata) {
            auto it = metadata->find("authorization");
            if (it == metadata->end())
                it = metadata->find("grpcgateway-authorization");
            if (it != metadata->end()) {
                std::string val(it->second.data(), it->second.size());
                const std::string prefix = "Bearer ";
                if (val.size() > prefix.size() && val.substr(0, prefix.size()) == prefix) {
                    std::string token = val.substr(prefix.size());
                    g_rpc_auth_ctx = AuthInterceptor::FromToken(token, jwt_secret_);
                    found = true;
                }
            }
        }
        if (!found)
            g_rpc_auth_ctx = {};
    }
    methods->Proceed();
}

RpcAuthInterceptorFactory::RpcAuthInterceptorFactory(std::string jwt_secret)
    : jwt_secret_(std::move(jwt_secret)) {}

grpc::experimental::Interceptor *RpcAuthInterceptorFactory::CreateServerInterceptor() {
    return new RpcAuthInterceptor(jwt_secret_);
}
