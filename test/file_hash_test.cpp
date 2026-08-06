/**
 * @file file_hash_test.cpp
 * @brief FileHasher 流式哈希冒烟测试（无第三方测试框架）
 *
 * =============================================================================
 * 覆盖点
 * =============================================================================
 * 1. Update + FinalizeHex：内存缓冲区一次喂入
 * 2. 分块 Update：多次 Update 结果应与一次喂入相同（流式语义）
 * 3. HashFile：按块读文件，结果应与内存计算一致
 *
 * =============================================================================
 * 用法
 * =============================================================================
 *   ./file_hash_test              # 跑上述三项，对照已知 SHA-256
 *   ./file_hash_test /path/file   # 工具模式：打印该文件的 SHA-256 十六进制
 *
 * 期望摘要可用系统命令核对：
 *   echo -n "abc" | sha256sum
 *   # ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 *
 * 注意：echo 默认带换行；核对时需 echo -n，或 printf 'abc'。
 */

#include "FileHash.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

/**
 * 断言 got == want：成功打 OK，失败打 FAIL 并打印两侧摘要。
 * @return true 表示通过
 */
bool ExpectEq(const char *name, const std::string &got, const std::string &want) {
    if (got == want) {
        std::printf("OK  %s\n", name);
        return true;
    }
    std::fprintf(stderr, "FAIL %s\ngot  %s\nwant %s\n", name, got.c_str(), want.c_str());
    return false;
}

}  // namespace

int main(int argc, char *argv[]) {
    // -------------------------------------------------------------------------
    // 工具模式：带路径参数时只算文件哈希并打印（格式接近 sha256sum）
    // -------------------------------------------------------------------------
    if (argc >= 2) {
        const std::string path = argv[1];
        // 默认 Algo::Sha256，内部按 64KB 块流式读，不整文件进内存
        const std::string hex = FileHasher::HashFile(path);
        if (hex.empty()) {
            // 打开失败、读错误或 OpenSSL 失败都会得到空串
            std::fprintf(stderr, "FAIL HashFile %s\n", path.c_str());
            return 1;
        }
        std::printf("%s  %s\n", hex.c_str(), path.c_str());
        return 0;
    }

    // -------------------------------------------------------------------------
    // 自测模式：固定明文 "abc" 的标准 SHA-256（FIPS 180 / 常见测试向量）
    // -------------------------------------------------------------------------
    // 等价命令：echo -n "abc" | sha256sum
    const std::string kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    // --- 用例 1：单次 Update 后 Finalize ---
    FileHasher h(FileHasher::Algo::Sha256);
    if (!h.Update("abc", 3)) {
        std::fprintf(stderr, "FAIL Update\n");
        return 1;
    }
    // FinalizeHex 之后不可再 Update；返回小写十六进制
    const std::string streamed = h.FinalizeHex();
    if (!ExpectEq("stream Update+Finalize", streamed, kAbcSha256))
        return 1;

    // --- 用例 2：分块 Update，验证流式拼接与一次喂入等价 ---
    // 这是「流式哈希」的核心性质：H(a||b||c) == Update(a);Update(b);Update(c);Finalize
    FileHasher h2(FileHasher::Algo::Sha256);
    h2.Update("a", 1);
    h2.Update("b", 1);
    h2.Update("c", 1);
    if (!ExpectEq("chunked Update", h2.FinalizeHex(), kAbcSha256))
        return 1;

    // --- 用例 3：落盘后再 HashFile，验证文件路径入口 ---
    // 使用 binary 模式写入，内容仅为三个字节 'a''b''c'，无额外换行
    const char *tmp = "/tmp/file_hash_test_abc.txt";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "abc";
    }
    if (!ExpectEq("HashFile", FileHasher::HashFile(tmp), kAbcSha256))
        return 1;

    std::printf("PASS file_hash_test\n");
    return 0;
}
