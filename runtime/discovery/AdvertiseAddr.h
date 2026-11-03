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

/** 从 listen 生成对端能拨通的地址。
 *  0.0.0.0 / :: 换成 GAMEMESH_ADVERTISE_HOST。
 *  已经绑在具体地址（含 127.0.0.1）上时必须原样公布，否则对端会去连一块没在听的网卡。
 */
inline std::string AdvertiseFromListen(const std::string &listen_addr) {
    const int port = PortFromHostPort(listen_addr);
    if (port <= 0)
        return {};
    const auto colon = listen_addr.rfind(':');
    std::string host = colon == std::string::npos ? listen_addr : listen_addr.substr(0, colon);
    if (!host.empty() && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (!IsForbiddenAdvertiseHost(host))
        return host + ":" + std::to_string(port);
    return MakeAdvertiseAddr(port);
}
