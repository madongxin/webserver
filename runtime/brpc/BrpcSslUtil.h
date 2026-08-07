#pragma once

/**
 * @file BrpcSslUtil.h
 * @brief 可选 mTLS：从 *.cnf 读 ssl_enable/ssl_cert/ssl_key/ssl_ca，写入 brpc options
 *
 * 注意：包含本头之前请先包含项目 Logging/common，或接受 butil 的 DISALLOW_COPY 定义。
 */

#include <brpc/channel.h>
#include <brpc/server.h>

#include <fstream>
#include <string>

namespace BrpcSslUtil {

inline std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

struct SslFiles {
    bool enable = false;
    std::string cert;
    std::string key;
    std::string ca;
};

inline void LoadFromCnf(const std::string &path, SslFiles *out) {
    if (!out)
        return;
    std::ifstream in(path);
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "ssl_enable")
            out->enable = (val == "1" || val == "true");
        else if (key == "ssl_cert")
            out->cert = val;
        else if (key == "ssl_key")
            out->key = val;
        else if (key == "ssl_ca")
            out->ca = val;
    }
}

/** 启用服务端 SSL；失败（缺证书）返回 false 且不改 options */
inline bool ApplyServer(brpc::ServerOptions *options, const SslFiles &ssl) {
    if (!options || !ssl.enable)
        return false;
    if (ssl.cert.empty() || ssl.key.empty())
        return false;
    brpc::ServerSSLOptions *ssl_opt = options->mutable_ssl_options();
    ssl_opt->default_cert.certificate = ssl.cert;
    ssl_opt->default_cert.private_key = ssl.key;
    if (!ssl.ca.empty()) {
        ssl_opt->verify.ca_file_path = ssl.ca;
        ssl_opt->verify.verify_depth = 1;
    }
    return true;
}

inline bool ApplyChannel(brpc::ChannelOptions *options, const SslFiles &ssl) {
    if (!options || !ssl.enable)
        return false;
    brpc::ChannelSSLOptions *ssl_opt = options->mutable_ssl_options();
    if (!ssl.ca.empty()) {
        ssl_opt->verify.ca_file_path = ssl.ca;
        ssl_opt->verify.verify_depth = 1;
    }
    if (!ssl.cert.empty() && !ssl.key.empty()) {
        ssl_opt->client_cert.certificate = ssl.cert;
        ssl_opt->client_cert.private_key = ssl.key;
    }
    return true;
}

}  // namespace BrpcSslUtil
