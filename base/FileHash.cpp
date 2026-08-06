/**
 * @file FileHash.cpp
 * @brief OpenSSL EVP 流式哈希实现
 */

#include "FileHash.h"

#include <openssl/evp.h>

#include <cstdio>
#include <fstream>
#include <vector>

namespace {

const EVP_MD *MdForAlgo(FileHasher::Algo algo) {
    switch (algo) {
    case FileHasher::Algo::Sha256:
        return EVP_sha256();
    case FileHasher::Algo::Sha1:
        return EVP_sha1();
    case FileHasher::Algo::Md5:
        return EVP_md5();
    }
    return EVP_sha256();
}

std::string ToHex(const unsigned char *data, unsigned int len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(static_cast<size_t>(len) * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[data[i] & 0xf];
    }
    return out;
}

}  // namespace

bool FileHasher::Init(Algo algo) {
    const EVP_MD *md = MdForAlgo(algo);
    if (!md)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return false;
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    ctx_ = ctx;
    finished_ = false;
    return true;
}

FileHasher::FileHasher(Algo algo) {
    Init(algo);
}

FileHasher::~FileHasher() {
    if (ctx_) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(ctx_));
        ctx_ = nullptr;
    }
}

bool FileHasher::Update(const void *data, size_t len) {
    if (!ctx_ || finished_)
        return false;
    if (len == 0)
        return true;
    if (!data)
        return false;
    return EVP_DigestUpdate(static_cast<EVP_MD_CTX *>(ctx_), data, len) == 1;
}

std::string FileHasher::FinalizeHex() {
    if (!ctx_ || finished_)
        return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX *>(ctx_), digest, &digest_len) != 1)
        return {};
    finished_ = true;
    return ToHex(digest, digest_len);
}

std::string FileHasher::HashStream(std::istream &in, Algo algo, size_t chunk_size) {
    if (!in || chunk_size == 0)
        return {};
    FileHasher h(algo);
    if (!h.ctx_)
        return {};
    std::vector<char> buf(chunk_size);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(chunk_size));
        const std::streamsize n = in.gcount();
        if (n > 0) {
            if (!h.Update(buf.data(), static_cast<size_t>(n)))
                return {};
        }
    }
    // failbit 在 EOF 时也会置位；仅当读到数据前就坏掉才算失败
    if (in.bad())
        return {};
    return h.FinalizeHex();
}

std::string FileHasher::HashFile(const std::string &path, Algo algo, size_t chunk_size) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return HashStream(in, algo, chunk_size);
}
