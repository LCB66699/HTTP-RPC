#pragma once
#include <string>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace jwt {

inline std::string base64url_encode(const std::string& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    int val = 0, valb = -6;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.data());
    for (size_t i = 0; i < data.size(); ++i) {
        val = (val << 8) + bytes[i];
        valb += 8;
        while (valb >= 0) { out.push_back(tbl[(val >> valb) & 0x3F]); valb -= 6; }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline std::string base64url_decode(std::string data) {
    static unsigned char tbl[256] = {};
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) tbl[i] = 0xFF;
        for (int i = 0; i < 64; ++i) {
            const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            tbl[(unsigned char)alphabet[i]] = (unsigned char)i;
        }
        tbl[(unsigned char)'-'] = tbl[(unsigned char)'+'];
        tbl[(unsigned char)'_'] = tbl[(unsigned char)'/'];
        init = true;
    }
    while (!data.empty() && data.back() == '=') data.pop_back();
    std::string out;
    out.reserve(data.size() * 3 / 4);
    int val = 0, valb = -8;
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char d = tbl[(unsigned char)data[i]];
        if (d == 0xFF) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) { out.push_back((char)((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

inline std::string hmac_sha256(const std::string& data, const std::string& key) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         (const unsigned char*)data.data(), data.size(), result, &len);
    return std::string((char*)result, len);
}

inline std::string create(const std::string& payload_json, const std::string& secret) {
    std::string header = base64url_encode("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    std::string payload = base64url_encode(payload_json);
    std::string sig = base64url_encode(hmac_sha256(header + "." + payload, secret));
    return header + "." + payload + "." + sig;
}

inline bool verify(const std::string& token, const std::string& secret, std::string& payload_out) {
    size_t p1 = token.find('.');
    size_t p2 = token.find('.', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) return false;

    std::string msg = token.substr(0, p2);
    std::string sig_b64 = token.substr(p2 + 1);
    // 恒定时间比较，防时序攻击
    auto constant_time_eq = [](const std::string& a, const std::string& b) -> bool {
        if (a.size() != b.size()) return false;
        unsigned char r = 0;
        for (size_t i = 0; i < a.size(); ++i) r |= a[i] ^ b[i];
        return r == 0;
    };
    std::string expected = base64url_encode(hmac_sha256(msg, secret));
    if (!constant_time_eq(expected, sig_b64)) return false;

    payload_out = base64url_decode(token.substr(p1 + 1, p2 - p1 - 1));
    return true;
}

} // namespace jwt
