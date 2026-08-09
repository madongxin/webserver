#pragma once

#include <sstream>
#include <string>
#include <vector>

/** 构造 brpc NamingService 多地址：list://host:port,host:port */
inline std::string BuildListNamingUrl(const std::vector<std::string> &addrs) {
    if (addrs.empty())
        return {};
    if (addrs.size() == 1) {
        // 单地址仍可用裸 host:port；多地址用 list:// + rr
        if (addrs[0].rfind("list://", 0) == 0)
            return addrs[0];
        return addrs[0];
    }
    std::ostringstream os;
    os << "list://";
    for (size_t i = 0; i < addrs.size(); ++i) {
        if (i)
            os << ',';
        std::string a = addrs[i];
        if (a.rfind("list://", 0) == 0)
            a = a.substr(7);
        os << a;
    }
    return os.str();
}

inline std::string SessionLoadBalancerName(size_t addr_count) {
    return addr_count > 1 ? "rr" : "";
}
