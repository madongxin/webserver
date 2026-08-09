#include "PasswordHash.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstdio>
#include <vector>

namespace {

std::string ToHex(const unsigned char *data, size_t n) {
    static const char *kHex = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[data[i] & 0xf];
    }
    return out;
}

bool FromHex(const std::string &hex, std::vector<unsigned char> *out) {
    if (!out || hex.size() % 2 != 0)
        return false;
    out->resize(hex.size() / 2);
    for (size_t i = 0; i < out->size(); ++i) {
        unsigned int b = 0;
        if (std::sscanf(hex.c_str() + i * 2, "%2x", &b) != 1)
            return false;
        (*out)[i] = static_cast<unsigned char>(b);
    }
    return true;
}

}  // namespace

namespace PasswordHash {

bool GenerateSalt(std::string *salt_hex) {
    if (!salt_hex)
        return false;
    unsigned char buf[kSaltBytes];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        return false;
    *salt_hex = ToHex(buf, sizeof(buf));
    return true;
}

bool HashPassword(const std::string &password, const std::string &salt_hex, int iterations,
                  std::string *hash_hex) {
    if (!hash_hex || salt_hex.empty() || iterations < 1000)
        return false;
    std::vector<unsigned char> salt;
    if (!FromHex(salt_hex, &salt))
        return false;
    unsigned char out[kHashBytes];
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(),
                          static_cast<int>(sizeof(out)), out) != 1)
        return false;
    *hash_hex = ToHex(out, sizeof(out));
    return true;
}

bool VerifyPassword(const std::string &password, const std::string &salt_hex, int iterations,
                    const std::string &expected_hash_hex) {
    std::string got;
    if (!HashPassword(password, salt_hex, iterations, &got))
        return false;
    if (got.size() != expected_hash_hex.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < got.size(); ++i)
        diff |= static_cast<unsigned char>(got[i] ^ expected_hash_hex[i]);
    return diff == 0;
}

std::string RedactSecret(const std::string &s) {
    if (s.size() <= 4)
        return "****";
    return s.substr(0, 2) + "****" + s.substr(s.size() - 2);
}

bool Sha256Hex(const std::string &data, std::string *hex_out) {
    if (!hex_out)
        return false;
    unsigned char md[SHA256_DIGEST_LENGTH];
    if (!SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), md))
        return false;
    static const char hex[] = "0123456789abcdef";
    hex_out->assign(SHA256_DIGEST_LENGTH * 2, '0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        (*hex_out)[static_cast<size_t>(i) * 2] = hex[md[i] >> 4];
        (*hex_out)[static_cast<size_t>(i) * 2 + 1] = hex[md[i] & 0xf];
    }
    return true;
}

}  // namespace PasswordHash
