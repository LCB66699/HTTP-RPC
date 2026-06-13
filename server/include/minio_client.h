// Minimal MinIO/S3 client — implements PutObject and PresignedGetUrl
// using AWS Signature Version 4 over HTTP(S).
// Depends only on OpenSSL (already linked for gRPC) and httplib.h.
#pragma once
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include "httplib.h"

namespace minio {

// ---- Crypto helpers -------------------------------------------------------

inline std::string sha256hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    std::ostringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return ss.str();
}

// Raw HMAC-SHA256 bytes
inline std::string hmac256(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &len);
    return std::string(reinterpret_cast<char*>(result), len);
}

inline std::string hmac256hex(const std::string& key, const std::string& data) {
    std::string h = hmac256(key, data);
    std::ostringstream ss;
    for (unsigned char c : h)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return ss.str();
}

// ---- Time helpers ---------------------------------------------------------

inline void get_datetime(std::string& date8, std::string& datetime16) {
    time_t now = time(nullptr);
    struct tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d", &utc);
    date8 = buf;
    strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
    datetime16 = buf;
}

// ---- URL encoding ---------------------------------------------------------

// Encodes every byte except unreserved chars and '/' (kept for path segments).
inline std::string url_encode_path(const std::string& s) {
    std::ostringstream enc;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
            enc << c;
        else
            enc << '%' << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0') << (int)c;
    }
    return enc.str();
}

// Encodes every byte except unreserved chars (no '/').
inline std::string url_encode(const std::string& s) {
    std::ostringstream enc;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            enc << c;
        else
            enc << '%' << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0') << (int)c;
    }
    return enc.str();
}

// Authority (host or host:port) for SigV4 — must match the Host header the browser
// sends when opening the presigned URL, not the internal Docker endpoint.
inline std::string signing_host_from_public_url(const std::string& public_url) {
    if (public_url.empty()) return {};
    size_t pos = 0;
    if (public_url.rfind("https://", 0) == 0)
        pos = 8;
    else if (public_url.rfind("http://", 0) == 0)
        pos = 7;
    else
        return {};
    size_t slash = public_url.find('/', pos);
    if (slash == std::string::npos)
        return public_url.substr(pos);
    return public_url.substr(pos, slash - pos);
}

// ---- Client ---------------------------------------------------------------

struct Client {
    std::string endpoint;    // host:port, e.g. "minio:9000"
    std::string public_url;  // external base URL for presigned links, e.g. "http://localhost:9000"
    std::string access_key;
    std::string secret_key;
    std::string bucket;
    std::string region = "us-east-1";  // MinIO default

    // Returns false on any HTTP/network error.
    bool PutObject(const std::string& key, const std::string& data,
                   const std::string& content_type = "application/octet-stream") const {
        std::string date8, dt16;
        get_datetime(date8, dt16);

        std::string host = endpoint;
        std::string uri  = "/" + bucket + "/" + url_encode_path(key);
        std::string payload_hash = sha256hex(data);

        // Canonical headers must be in sorted order.
        std::string canon_headers =
            "content-type:" + content_type + "\n"
            "host:" + host + "\n"
            "x-amz-content-sha256:" + payload_hash + "\n"
            "x-amz-date:" + dt16 + "\n";
        std::string signed_headers = "content-type;host;x-amz-content-sha256;x-amz-date";

        std::string canon_request =
            "PUT\n" + uri + "\n\n" +
            canon_headers + "\n" +
            signed_headers + "\n" +
            payload_hash;

        std::string scope = date8 + "/" + region + "/s3/aws4_request";
        std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" + dt16 + "\n" + scope + "\n" +
            sha256hex(canon_request);

        std::string signing_key = hmac256(
            hmac256(hmac256(hmac256("AWS4" + secret_key, date8), region), "s3"),
            "aws4_request");
        std::string signature = hmac256hex(signing_key, string_to_sign);

        std::string auth =
            "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
            ", SignedHeaders=" + signed_headers +
            ", Signature=" + signature;

        httplib::Client cli(endpoint);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(60);

        httplib::Headers headers = {
            {"Host",                   host},
            {"x-amz-date",             dt16},
            {"x-amz-content-sha256",   payload_hash},
            {"Authorization",          auth}
        };
        auto res = cli.Put(uri, headers, data, content_type);
        if (!res) {
            fprintf(stderr, "[MinIO] PutObject network error for key=%s\n", key.c_str());
            return false;
        }
        if (res->status != 200 && res->status != 201 && res->status != 204) {
            fprintf(stderr, "[MinIO] PutObject HTTP %d for key=%s: %s\n",
                    res->status, key.c_str(), res->body.c_str());
            return false;
        }
        return true;
    }

    // Authenticated GET object (SigV4) — reads from internal `endpoint` only.
    // Used so downloads can go gateway → gRPC → MinIO without browser → :9000/:443.
    bool GetObject(const std::string& key, std::string& out_body) const {
        std::string date8, dt16;
        get_datetime(date8, dt16);
        std::string host = endpoint;
        std::string uri  = "/" + bucket + "/" + url_encode_path(key);
        // Empty request body — standard hex(SHA256("")) used by AWS SigV4 for GET.
        const std::string payload_hash =
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

        std::string canon_headers =
            "host:" + host + "\n"
            "x-amz-content-sha256:" + payload_hash + "\n"
            "x-amz-date:" + dt16 + "\n";
        std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";

        std::string canon_request =
            "GET\n" + uri + "\n\n" +
            canon_headers + "\n" +
            signed_headers + "\n" +
            payload_hash;

        std::string scope = date8 + "/" + region + "/s3/aws4_request";
        std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" + dt16 + "\n" + scope + "\n" +
            sha256hex(canon_request);

        std::string signing_key = hmac256(
            hmac256(hmac256(hmac256("AWS4" + secret_key, date8), region), "s3"),
            "aws4_request");
        std::string signature = hmac256hex(signing_key, string_to_sign);

        std::string auth =
            "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
            ", SignedHeaders=" + signed_headers +
            ", Signature=" + signature;

        httplib::Client cli(endpoint);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(3600);

        httplib::Headers headers = {
            {"Host",                   host},
            {"x-amz-date",             dt16},
            {"x-amz-content-sha256",   payload_hash},
            {"Authorization",          auth}
        };
        auto res = cli.Get(uri, headers);
        if (!res) {
            fprintf(stderr, "[MinIO] GetObject network error for key=%s\n", key.c_str());
            return false;
        }
        if (res->status != 200) {
            fprintf(stderr, "[MinIO] GetObject HTTP %d for key=%s: %s\n",
                    res->status, key.c_str(), res->body.substr(0, 256).c_str());
            return false;
        }
        out_body = std::move(res->body);
        return true;
    }

    // DELETE object — S3 SigV4 signed, same pattern as GetObject.
    bool DeleteObject(const std::string& key) const {
        std::string date8, dt16;
        get_datetime(date8, dt16);
        std::string host = endpoint;
        std::string uri  = "/" + bucket + "/" + url_encode_path(key);
        const std::string payload_hash =
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

        std::string canon_headers =
            "host:" + host + "\n"
            "x-amz-content-sha256:" + payload_hash + "\n"
            "x-amz-date:" + dt16 + "\n";
        std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";

        std::string canon_request =
            "DELETE\n" + uri + "\n\n" +
            canon_headers + "\n" +
            signed_headers + "\n" +
            payload_hash;

        std::string scope = date8 + "/" + region + "/s3/aws4_request";
        std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" + dt16 + "\n" + scope + "\n" +
            sha256hex(canon_request);

        std::string signing_key = hmac256(
            hmac256(hmac256(hmac256("AWS4" + secret_key, date8), region), "s3"),
            "aws4_request");
        std::string signature = hmac256hex(signing_key, string_to_sign);

        std::string auth =
            "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
            ", SignedHeaders=" + signed_headers +
            ", Signature=" + signature;

        httplib::Client cli(endpoint);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);

        httplib::Headers headers = {
            {"Host",                   host},
            {"x-amz-date",             dt16},
            {"x-amz-content-sha256",   payload_hash},
            {"Authorization",          auth}
        };
        auto res = cli.Delete(uri, headers);
        if (!res) {
            fprintf(stderr, "[MinIO] DeleteObject network error for key=%s\n", key.c_str());
            return false;
        }
        if (res->status != 200 && res->status != 204 && res->status != 404) {
            fprintf(stderr, "[MinIO] DeleteObject HTTP %d for key=%s: %s\n",
                    res->status, key.c_str(), res->body.substr(0, 256).c_str());
            return false;
        }
        return true;
    }

    // Generates a pre-signed GET URL valid for `expires` seconds.
    // The URL uses `public_url` as the base so browser can reach MinIO directly.
    std::string PresignedGetUrl(const std::string& key, int expires = 3600) const {
        std::string date8, dt16;
        get_datetime(date8, dt16);

        std::string uri        = "/" + bucket + "/" + url_encode_path(key);
        std::string scope      = date8 + "/" + region + "/s3/aws4_request";
        std::string credential = access_key + "/" + scope;

        // Query string parameters must be sorted alphabetically.
        std::string query =
            "X-Amz-Algorithm=AWS4-HMAC-SHA256"
            "&X-Amz-Credential=" + url_encode(credential) +
            "&X-Amz-Date="       + dt16 +
            "&X-Amz-Expires="    + std::to_string(expires) +
            "&X-Amz-SignedHeaders=host";

        // Sign the host the *client* will send (e.g. localhost), not minio:9000.
        std::string host_for_sign = signing_host_from_public_url(public_url);
        if (host_for_sign.empty())
            host_for_sign = endpoint;
        std::string canon_request =
            "GET\n" + uri + "\n" + query + "\n"
            "host:" + host_for_sign + "\n\n"
            "host\n"
            "UNSIGNED-PAYLOAD";

        std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" + dt16 + "\n" + scope + "\n" +
            sha256hex(canon_request);

        std::string signing_key = hmac256(
            hmac256(hmac256(hmac256("AWS4" + secret_key, date8), region), "s3"),
            "aws4_request");
        std::string signature = hmac256hex(signing_key, string_to_sign);

        // Use public_url so the presigned link works from outside the Docker network.
        std::string base = public_url.empty() ? ("http://" + endpoint) : public_url;
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        return base + uri + "?" + query + "&X-Amz-Signature=" + signature;
    }

    bool IsConfigured() const {
        return !endpoint.empty() && !access_key.empty() && !secret_key.empty() && !bucket.empty();
    }
};

} // namespace minio
