#pragma once

#include <cstdlib>
#include <string>

/** advertise_addr 校验：禁止不可路由地址（listen 可为 0.0.0.0）。 */
inline bool IsForbiddenAdvertiseHost(const std::string &host) {
    if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]")
        return true;
    return false;
}

inline bool ValidateAdvertiseAddr(const std::string &host_port, std::string *err = nullptr) {
    if (host_port.empty()) {
        if (err)
            *err = "advertise_addr empty";
        return false;
    }
    std::string host = host_port;
    const auto colon = host_port.rfind(':');
    if (colon != std::string::npos)
        host = host_port.substr(0, colon);
    // strip IPv6 brackets
    if (!host.empty() && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (IsForbiddenAdvertiseHost(host)) {
        if (err)
            *err = "advertise_addr must be routable (got " + host_port + ")";
        return false;
    }
    return true;
}

/** host:port 取端口；失败返回 0 */
inline int PortFromHostPort(const std::string &host_port) {
    const auto colon = host_port.rfind(':');
    if (colon == std::string::npos || colon + 1 >= host_port.size())
        return 0;
    return std::atoi(host_port.c_str() + colon + 1);
}

/** GAMEMESH_ADVERTISE_HOST + port → host:port；缺省 127.0.0.1（开发） */
inline std::string MakeAdvertiseAddr(int port) {
    const char *adv = std::getenv("GAMEMESH_ADVERTISE_HOST");
    std::string host = (adv && *adv) ? adv : "127.0.0.1";
    if (IsForbiddenAdvertiseHost(host))
        host = "127.0.0.1";
    return host + ":" + std::to_string(port);
}

/** 从 listen（可为 0.0.0.0:port）生成可路由 advertise */
inline std::string AdvertiseFromListen(const std::string &listen_addr) {
    const int port = PortFromHostPort(listen_addr);
    if (port <= 0)
        return {};
    return MakeAdvertiseAddr(port);
}
