#pragma once
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <string>

namespace sha256 {

inline std::string hash_hex(const std::string &input) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, input.data(), input.size());
  EVP_DigestFinal_ex(ctx, digest, &digest_len);
  EVP_MD_CTX_free(ctx);

  std::ostringstream oss;
  for (unsigned int i = 0; i < digest_len; ++i)
    oss << std::hex << std::setfill('0') << std::setw(2) << (int)digest[i];
  return oss.str();
}

// 格式: pbkdf2_sha256$iterations$hex_salt$hex_hash
inline std::string hash_password(const std::string &password,
                                 int iterations = 100000) {
  unsigned char salt[16];
  if (!RAND_bytes(salt, sizeof(salt))) {
    // RAND_bytes 失败时回退到裸 SHA256（极度罕见）
    fprintf(stderr,
            "[sha256] RAND_bytes failed, falling back to bare SHA256\n");
    return "sha256$1$$" + hash_hex(password);
  }

  unsigned char key[32];
  PKCS5_PBKDF2_HMAC(password.data(), (int)password.size(), salt,
                    (int)sizeof(salt), iterations, EVP_sha256(),
                    (int)sizeof(key), key);

  std::ostringstream oss;
  oss << "pbkdf2_sha256$" << iterations << "$";
  for (int i = 0; i < (int)sizeof(salt); ++i)
    oss << std::hex << std::setfill('0') << std::setw(2) << (int)salt[i];
  oss << "$";
  for (int i = 0; i < (int)sizeof(key); ++i)
    oss << std::hex << std::setfill('0') << std::setw(2) << (int)key[i];
  return oss.str();
}

inline bool verify_password(const std::string &password,
                            const std::string &stored) {
  if (stored.empty())
    return false;

  // 兼容旧格式（纯hex = 裸SHA256）
  if (stored.find('$') == std::string::npos) {
    return hash_hex(password) == stored;
  }

  // 解析新格式: algorithm$iterations$salt$hash
  size_t p1 = stored.find('$');
  size_t p2 = stored.find('$', p1 + 1);
  size_t p3 = stored.find('$', p2 + 1);
  if (p1 == std::string::npos || p2 == std::string::npos ||
      p3 == std::string::npos)
    return false;

  std::string algo = stored.substr(0, p1);
  int iterations = std::stoi(stored.substr(p1 + 1, p2 - p1 - 1));
  std::string salt_hex = stored.substr(p2 + 1, p3 - p2 - 1);
  std::string hash_hex_stored = stored.substr(p3 + 1);

  // hex salt → bytes
  unsigned char salt[64];
  int salt_len = 0;
  for (size_t i = 0; i + 1 < salt_hex.size() && salt_len < (int)sizeof(salt);
       i += 2)
    salt[salt_len++] =
        (unsigned char)std::stoi(salt_hex.substr(i, 2), nullptr, 16);

  // hex hash → bytes
  unsigned char expected[64];
  int expected_len = 0;
  for (size_t i = 0;
       i + 1 < hash_hex_stored.size() && expected_len < (int)sizeof(expected);
       i += 2)
    expected[expected_len++] =
        (unsigned char)std::stoi(hash_hex_stored.substr(i, 2), nullptr, 16);

  // 重新计算
  unsigned char computed[64];
  int computed_len = (algo == "pbkdf2_sha256") ? 32 : 0;
  if (computed_len == 0)
    return false;

  PKCS5_PBKDF2_HMAC(password.data(), (int)password.size(), salt, salt_len,
                    iterations, EVP_sha256(), computed_len, computed);

  // 恒定时间比较
  if (computed_len != expected_len)
    return false;
  unsigned char r = 0;
  for (int i = 0; i < computed_len; ++i)
    r |= computed[i] ^ expected[i];
  return r == 0;
}

} // namespace sha256
