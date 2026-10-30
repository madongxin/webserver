#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/** 正式模式默认只允许 loopback 和内网地址。0.0.0.0 与公网地址需要显式开关。 */
struct ListenBindDecision {
    bool ok = true;
    std::string error;
};

inline std::string ListenHostOf(const std::string &addr) {
    if (addr.empty())
        return {};
    if (addr[0] == '[') {
        const auto end = addr.find(']');
        if (end != std::string::npos)
            return addr.substr(1, end - 1);
    }
    const auto first = addr.find(':');
    const auto last = addr.rfind(':');
    if (first != std::string::npos && first == last)
        return addr.substr(0, first);
    return addr;
}

inline bool Ipv4LoopbackOrPrivate(const std::string &host) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    if (a == 127 || a == 10)
        return true;
    if (a == 192 && b == 168)
        return true;
    if (a == 172 && b >= 16 && b <= 31)
        return true;
    if (a == 169 && b == 254)
        return true;
    return false;
}

inline ListenBindDecision EvaluateListenBind(const std::string &listen_addr, bool formal,
                                            bool allow_public_bind) {
    ListenBindDecision out;
    const std::string host = ListenHostOf(listen_addr);
    if (host.empty()) {
        out.ok = false;
        out.error = "listen address is empty";
        return out;
    }
    if (!formal || allow_public_bind)
        return out;
    if (host == "localhost" || host == "::1" || Ipv4LoopbackOrPrivate(host))
        return out;
    out.ok = false;
    out.error = "formal mode refuses listen on " + listen_addr +
                " without allow_public_bind=true";
    return out;
}

inline bool AllowPublicBindFromEnv() {
    const char *v = std::getenv("GAMEMESH_ALLOW_PUBLIC_BIND");
    if (!v || !*v)
        return false;
    return std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "TRUE") == 0;
}

inline bool ConfigFlagTrue(const std::string &val) {
    return val == "1" || val == "true" || val == "TRUE" || val == "yes";
}
