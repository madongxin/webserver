#include "SecureRandom.h"

#include <openssl/rand.h>

#include <vector>

namespace SecureRandom {

bool Fill(void *buf, size_t n) {
    if (!buf || n == 0)
        return false;
    return RAND_bytes(static_cast<unsigned char *>(buf), static_cast<int>(n)) == 1;
}

bool Hex(size_t nchars, std::string *out) {
    if (!out || nchars == 0)
        return false;
    const size_t nbytes = (nchars + 1) / 2;
    std::vector<unsigned char> raw(nbytes);
    if (!Fill(raw.data(), raw.size()))
        return false;
    static const char kHex[] = "0123456789abcdef";
    out->assign(nchars, '0');
    for (size_t i = 0; i < nchars; ++i) {
        const unsigned char b = raw[i / 2];
        (*out)[i] = kHex[(i % 2 == 0) ? ((b >> 4) & 0xf) : (b & 0xf)];
    }
    return true;
}

}  // namespace SecureRandom
