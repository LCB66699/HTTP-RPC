#pragma once
#include <grpcpp/grpcpp.h>

#include <string>

#include "generated/rpc_search.grpc.pb.h"
#include "generated/rpc_search.pb.h"

class SearchServiceImpl final : public rpc::SearchService::Service {
   public:
    explicit SearchServiceImpl(const std::string &es_host) : es_host_(es_host) {}

    grpc::Status Search(grpc::ServerContext *ctx, const rpc::SearchRequest *req, rpc::SearchResponse *resp) override;

   private:
    std::string es_host_;
};
