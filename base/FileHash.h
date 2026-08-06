#pragma once

/**
 * @file FileHash.h
 * @brief 流式文件/内存哈希（OpenSSL EVP：SHA-256 / SHA-1 / MD5）
 *
 * 用法（流式，适合大文件，不整文件读入内存）：
 *
 *   FileHasher h(FileHasher::Algo::Sha256);
 *   h.Update(buf, n);          // 可多次
 *   std::string hex = h.FinalizeHex();
 *
 *   // 或一步到位：
 *   auto hex = FileHasher::HashFile("/path/to/file");
 *   auto hex2 = FileHasher::HashStream(istream);
 */

#include <cstddef>
#include <istream>
#include <string>

class FileHasher {
public:
    enum class Algo { Sha256, Sha1, Md5 };

    explicit FileHasher(Algo algo = Algo::Sha256);
    ~FileHasher();

    FileHasher(const FileHasher &) = delete;
    FileHasher &operator=(const FileHasher &) = delete;

    /** 追加一段数据；Finalize 之后不可再 Update */
    bool Update(const void *data, size_t len);
    bool Update(const std::string &s) { return Update(s.data(), s.size()); }

    /** 结束计算，返回小写十六进制摘要；失败返回空串 */
    std::string FinalizeHex();

    /** 是否已 Finalize（或构造失败） */
    bool ok() const { return ctx_ != nullptr && !finished_; }

    /** 按块读取文件并计算哈希（默认 64KB 块） */
    static std::string HashFile(const std::string &path, Algo algo = Algo::Sha256,
                               size_t chunk_size = 64 * 1024);

    /** 从任意 istream 流式计算（直至 EOF） */
    static std::string HashStream(std::istream &in, Algo algo = Algo::Sha256,
                                 size_t chunk_size = 64 * 1024);

private:
    bool Init(Algo algo);

    void *ctx_ = nullptr;  // EVP_MD_CTX*
    bool finished_ = false;
};
