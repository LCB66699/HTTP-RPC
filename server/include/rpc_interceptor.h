#pragma once
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

#include "auth_interceptor.h"

// Set by RpcAuthInterceptor before handler executes.
// Handlers read this instead of calling AuthInterceptor::Authenticate().
extern thread_local AuthContext g_rpc_auth_ctx;

// gRPC ServerInterceptor — verifies JWT from client metadata at transport level.
// On success: populates g_rpc_auth_ctx and proceeds.
// On failure: resets g_rpc_auth_ctx (handler returns UNAUTHENTICATED).
class RpcAuthInterceptor : public grpc::experimental::Interceptor {
   public:
    explicit RpcAuthInterceptor(const std::string &jwt_secret);
    void Intercept(grpc::experimental::InterceptorBatchMethods *methods) override;

   private:
    std::string jwt_secret_;
};

class RpcAuthInterceptorFactory : public grpc::experimental::ServerInterceptorFactoryInterface {
   public:
    explicit RpcAuthInterceptorFactory(std::string jwt_secret);
    grpc::experimental::Interceptor *CreateServerInterceptor() override;

   private:
    std::string jwt_secret_;
};
