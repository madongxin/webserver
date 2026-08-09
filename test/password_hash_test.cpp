#include "PasswordHash.h"

#include <cstdio>
#include <string>

static int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

int main() {
    std::string salt, hash;
    if (!PasswordHash::GenerateSalt(&salt) || salt.size() != 32)
        return Fail("salt");
    if (!PasswordHash::HashPassword("secret-pass", salt, PasswordHash::kDefaultIterations, &hash))
        return Fail("hash");
    if (!PasswordHash::VerifyPassword("secret-pass", salt, PasswordHash::kDefaultIterations, hash))
        return Fail("verify ok");
    if (PasswordHash::VerifyPassword("wrong", salt, PasswordHash::kDefaultIterations, hash))
        return Fail("verify reject");
    const std::string red = PasswordHash::RedactSecret("abcdefgh");
    if (red.find("abcd") != std::string::npos && red.find("****") == std::string::npos)
        return Fail("redact");
    std::string dig;
    if (!PasswordHash::Sha256Hex("abc", &dig) || dig.size() != 64)
        return Fail("sha256");
    std::printf("PASS password_hash_test\n");
    return 0;
}
