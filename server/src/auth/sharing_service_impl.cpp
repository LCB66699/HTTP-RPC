#include "auth/sharing_service_impl.h"

#include <nlohmann/json.hpp>

#include "shared/client/database.h"
#include "shared/base/error_codes.h"

grpc::Status SharingServiceImpl::Share(grpc::ServerContext *ctx, const rpc::ShareRequest *req,
                                        rpc::ShareResponse *resp) {
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    db_->EnsureSharingTables();

    bool ok = db_->CreateResourceShare(req->owner_id(), req->resource_type(),
                                        req->resource_id(), req->grantee_username(),
                                        req->permission());
    resp->set_success(ok);
    if (!ok) {
        resp->set_error("Failed to create share");
        resp->set_error_code(rpc_error::INTERNAL);
    }
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::Revoke(grpc::ServerContext *ctx, const rpc::RevokeRequest *req,
                                         rpc::RevokeResponse *resp) {
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    bool ok = db_->RevokeResourceShare(req->owner_id(), req->resource_type(),
                                        req->resource_id(), req->grantee_username());
    resp->set_success(ok);
    if (!ok) {
        resp->set_error("Share not found or failed to revoke");
        resp->set_error_code(rpc_error::NOT_FOUND);
    }
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::ListShares(grpc::ServerContext *ctx, const rpc::ResourceRequest *req,
                                             rpc::ShareListResponse *resp) {
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    db_->EnsureSharingTables();

    std::string entries_json;
    bool ok = db_->ListResourceShares(req->owner_id(), req->resource_type(),
                                       req->resource_id(), entries_json);
    if (ok && !entries_json.empty() && entries_json != "[]") {
        auto j = nlohmann::json::parse(entries_json);
        for (auto &entry : j) {
            auto *ae = resp->add_entries();
            ae->set_username(entry.value("username", ""));
            ae->set_permission(entry.value("permission", ""));
            ae->set_granted_at(entry.value("granted_at", ""));
        }
    }
    resp->set_success(ok);
    if (!ok) {
        resp->set_error("Failed to list shares");
        resp->set_error_code(rpc_error::INTERNAL);
    }
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::CreateShareLink(grpc::ServerContext *ctx, const rpc::ShareLinkRequest *req,
                                                  rpc::ShareLinkResponse *resp) {
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    db_->EnsureSharingTables();

    std::string token;
    bool ok = db_->CreateShareLink(req->owner_id(), req->resource_type(),
                                    req->resource_id(), req->permission(), token);
    resp->set_success(ok);
    if (ok) {
        resp->set_token(token);
    } else {
        resp->set_error("Failed to create share link");
        resp->set_error_code(rpc_error::INTERNAL);
    }
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::GetByToken(grpc::ServerContext *ctx, const rpc::ShareTokenRequest *req,
                                             rpc::SharedResourceResponse *resp) {
    if (!db_) {
        resp->set_success(false);
        resp->set_error("Database not available");
        resp->set_error_code(rpc_error::INTERNAL);
        return grpc::Status::OK;
    }

    std::string resource_type, permission;
    int64_t resource_id = 0, owner_id = 0;
    bool ok = db_->GetShareLinkByToken(req->token(), resource_type, resource_id, permission, owner_id);
    resp->set_success(ok);
    if (ok) {
        auto *info = resp->mutable_info();
        info->set_resource_type(resource_type);
        info->set_resource_id(resource_id);
        info->set_permission(permission);
    } else {
        resp->set_error("Invalid or expired share token");
        resp->set_error_code(rpc_error::NOT_FOUND);
    }
    return grpc::Status::OK;
}
