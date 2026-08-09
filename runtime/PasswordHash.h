#pragma once

#include <string>

/** PBKDF2-HMAC-SHA256 密码哈希（OpenSSL）；日志中禁止打印明文或完整 hash。 */
namespace PasswordHash {

constexpr int kDefaultIterations = 100000;
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;

bool GenerateSalt(std::string *salt_hex);
bool HashPassword(const std::string &password, const std::string &salt_hex, int iterations,
                  std::string *hash_hex);
bool VerifyPassword(const std::string &password, const std::string &salt_hex, int iterations,
                    const std::string &expected_hash_hex);

/** 脱敏：仅保留前后各 2 字符 */
std::string RedactSecret(const std::string &s);

/** SHA-256 hex（小写），用于 token 摘要 / 幂等键材料 */
bool Sha256Hex(const std::string &data, std::string *hex_out);

}  // namespace PasswordHash
