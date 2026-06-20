#include "rpc_interceptor.h"

thread_local AuthContext g_rpc_auth_ctx{};

RpcAuthInterceptor::RpcAuthInterceptor(const std::string &jwt_secret) : jwt_secret_(jwt_secret) {}

void RpcAuthInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods *methods) {
    if (methods->QueryInterceptionHookPoint(
            grpc::experimental::InterceptionHookPoints::PRE_SEND_INITIAL_METADATA)) {
        AuthInterceptor inner(jwt_secret_);
        AuthContext ac = inner.Authenticate(methods->GetServerContext());
        if (ac.authenticated) {
            g_rpc_auth_ctx = ac;
        } else {
            g_rpc_auth_ctx = {};
        }
    }
    methods->Proceed();
}

RpcAuthInterceptorFactory::RpcAuthInterceptorFactory(std::string jwt_secret)
    : jwt_secret_(std::move(jwt_secret)) {}

grpc::experimental::Interceptor *RpcAuthInterceptorFactory::CreateServerInterceptor() {
    return new RpcAuthInterceptor(jwt_secret_);
}
