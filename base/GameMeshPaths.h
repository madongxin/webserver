#pragma once

#include <cstdlib>
#include <string>
#include <unistd.h>

// 将 static / files 目录解析为可执行文件所在 build/test/ 上两级下的目录，
// 避免因启动 shell 的当前工作目录不同而出现空白页或上传路径错误。

namespace GameMeshPaths {

inline bool ResolveProjectSubdir(const char *subdir, std::string *out) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = '\0';
    // 运行中的二进制被重新编译替换后，内核会让 readlink 返回
    // ".../session (deleted)"。realpath 拒绝这个字符串，后续每次读配置都会失败。
    // 去掉后缀后指向同目录的新文件，项目根仍然是 build/test 的上两级。
    std::string exe_link(buf, static_cast<std::size_t>(n));
    static const char kDeleted[] = " (deleted)";
    const std::size_t deleted_len = sizeof(kDeleted) - 1;
    if (exe_link.size() >= deleted_len &&
        exe_link.compare(exe_link.size() - deleted_len, deleted_len, kDeleted) == 0) {
        exe_link.resize(exe_link.size() - deleted_len);
    }
    char exe_abs[4096];
    if (!realpath(exe_link.c_str(), exe_abs)) return false;
    std::string exe(exe_abs);
    std::size_t slash = exe.rfind('/');
    if (slash == std::string::npos) return false;
    std::string exe_dir = exe.substr(0, slash);
    char resolved[4096];
    std::string cand = exe_dir + "/../../" + subdir;
    if (!realpath(cand.c_str(), resolved)) return false;
    *out = resolved;
    return true;
}

inline const std::string &StaticRoot() {
    static std::string dir = "../static";
    static bool once = false;
    if (!once) {
        once = true;
        std::string r;
        if (ResolveProjectSubdir("static", &r)) dir = std::move(r);
    }
    return dir;
}

inline const std::string &FilesRoot() {
    static std::string dir = "../files";
    static bool once = false;
    if (!once) {
        once = true;
        std::string r;
        if (ResolveProjectSubdir("files", &r)) dir = std::move(r);
    }
    return dir;
}

}  // namespace GameMeshPaths
