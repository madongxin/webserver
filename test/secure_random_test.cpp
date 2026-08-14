/**
 * SecureRandom：CSPRNG fail-closed，禁止弱随机回退。
 */
#include "SecureRandom.h"

#include <cstdio>
#include <cstring>
#include <string>

int main() {
    if (SecureRandom::Fill(nullptr, 8)) {
        std::printf("FAIL null fill\n");
        return 1;
    }
    std::string a, b;
    if (!SecureRandom::Hex(32, &a) || !SecureRandom::Hex(32, &b)) {
        std::printf("FAIL hex\n");
        return 1;
    }
    if (a.size() != 32 || b.size() != 32 || a == b) {
        std::printf("FAIL hex uniqueness\n");
        return 1;
    }
    unsigned char buf[16];
    std::memset(buf, 0, sizeof(buf));
    if (!SecureRandom::Fill(buf, sizeof(buf))) {
        std::printf("FAIL fill\n");
        return 1;
    }
    bool nonzero = false;
    for (unsigned char c : buf)
        nonzero = nonzero || (c != 0);
    if (!nonzero) {
        std::printf("FAIL all-zero fill (extremely unlikely)\n");
        return 1;
    }
    std::printf("OK secure_random_test\n");
    return 0;
}
