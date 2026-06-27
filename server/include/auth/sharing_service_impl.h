#pragma once
#include <grpcpp/grpcpp.h>

#include "generated/rpc_sharing.grpc.pb.h"
#include "generated/rpc_sharing.pb.h"

class ShardedDatabase;

class SharingServiceImpl final : public rpc::SharingService::Service {
   public:
    explicit SharingServiceImpl(ShardedDatabase *db) : db_(db) {}

    grpc::Status Share(grpc::ServerContext *ctx, const rpc::ShareRequest *req,
                       rpc::ShareResponse *resp) override;
    grpc::Status Revoke(grpc::ServerContext *ctx, const rpc::RevokeRequest *req,
                        rpc::RevokeResponse *resp) override;
    grpc::Status ListShares(grpc::ServerContext *ctx, const rpc::ResourceRequest *req,
                            rpc::ShareListResponse *resp) override;
    grpc::Status CreateShareLink(grpc::ServerContext *ctx, const rpc::ShareLinkRequest *req,
                                 rpc::ShareLinkResponse *resp) override;
    grpc::Status GetByToken(grpc::ServerContext *ctx, const rpc::ShareTokenRequest *req,
                            rpc::SharedResourceResponse *resp) override;

   private:
    ShardedDatabase *db_ = nullptr;
};
