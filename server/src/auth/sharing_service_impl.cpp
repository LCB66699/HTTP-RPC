#include "auth/sharing_service_impl.h"

#include <openssl/rand.h>

#include <nlohmann/json.hpp>

#include "shared/base/rpc_interceptor.h"
#include "shared/client/database.h"
#include "shared/base/error_codes.h"

namespace {

template <typename Response>
grpc::Status Fail(Response *resp, const char *message, int error_code) {
    resp->set_success(false);
    resp->set_error(message);
    resp->set_error_code(error_code);
    return grpc::Status::OK;
}

bool HasPrincipal() {
    return g_rpc_auth_ctx.authenticated && g_rpc_auth_ctx.user_id > 0;
}

bool IsValidResourceType(const std::string &resource_type) {
    return resource_type == "sheet" || resource_type == "file";
}

bool IsValidPermission(const std::string &permission) {
    return permission == "view" || permission == "edit";
}

bool GenerateToken(std::string &out_token) {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return false;
    static constexpr char hex[] = "0123456789abcdef";
    out_token.resize(sizeof(bytes) * 2);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        out_token[i * 2] = hex[bytes[i] >> 4];
        out_token[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    return true;
}

template <typename Request, typename Response>
bool ValidateOwnedResource(const Request *req, Response *resp) {
    if (!HasPrincipal()) {
        Fail(resp, "Authentication required", rpc_error::UNAUTHENTICATED);
        return false;
    }
    if (req->owner_id() != g_rpc_auth_ctx.user_id) {
        Fail(resp, "Access denied", rpc_error::FORBIDDEN);
        return false;
    }
    if (req->resource_id() <= 0 || !IsValidResourceType(req->resource_type())) {
        Fail(resp, "Invalid shared resource", rpc_error::BAD_REQUEST);
        return false;
    }
    return true;
}

}  // namespace

grpc::Status SharingServiceImpl::Share(grpc::ServerContext *ctx, const rpc::ShareRequest *req,
                                        rpc::ShareResponse *resp) {
    if (!ValidateOwnedResource(req, resp)) return grpc::Status::OK;
    if (req->grantee_username().empty() || !IsValidPermission(req->permission())) {
        return Fail(resp, "Invalid sharing request", rpc_error::BAD_REQUEST);
    }
    if (!db_) {
        return Fail(resp, "Database not available", rpc_error::INTERNAL);
    }

    if (!db_->EnsureSharingTables()) return Fail(resp, "Database not available", rpc_error::INTERNAL);

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
    if (!ValidateOwnedResource(req, resp)) return grpc::Status::OK;
    if (req->grantee_username().empty()) return Fail(resp, "Invalid sharing request", rpc_error::BAD_REQUEST);
    if (!db_) {
        return Fail(resp, "Database not available", rpc_error::INTERNAL);
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
    if (!ValidateOwnedResource(req, resp)) return grpc::Status::OK;
    if (!db_) {
        return Fail(resp, "Database not available", rpc_error::INTERNAL);
    }

    if (!db_->EnsureSharingTables()) return Fail(resp, "Database not available", rpc_error::INTERNAL);

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
    if (!ValidateOwnedResource(req, resp)) return grpc::Status::OK;
    if (!IsValidPermission(req->permission())) return Fail(resp, "Invalid sharing request", rpc_error::BAD_REQUEST);
    if (!db_) {
        return Fail(resp, "Database not available", rpc_error::INTERNAL);
    }

    if (!db_->EnsureSharingTables()) return Fail(resp, "Database not available", rpc_error::INTERNAL);

    std::string token;
    const bool token_ready = GenerateToken(token);
    bool ok = token_ready && db_->CreateShareLink(req->owner_id(), req->resource_type(),
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

grpc::Status SharingServiceImpl::CheckAccess(grpc::ServerContext *ctx, const rpc::CheckAccessRequest *req,
                                              rpc::CheckAccessResponse *resp) {
    if (!db_) {
        resp->set_allowed(false);
        return grpc::Status::OK;
    }

    if (!db_->EnsureSharingTables()) return grpc::Status::OK;

    std::string username = db_->GetUsernameById(req->user_id());
    if (username.empty()) {
        resp->set_allowed(false);
        return grpc::Status::OK;
    }

    std::string perm;
    bool ok = db_->CheckShareAccess(username, req->resource_type(),
                                     req->resource_id(), perm);
    resp->set_allowed(ok);
    if (ok) resp->set_permission(perm);
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

    if (req->token().size() != 32) return Fail(resp, "Invalid share token", rpc_error::NOT_FOUND);
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
