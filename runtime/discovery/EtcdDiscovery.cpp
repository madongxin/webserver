#include "EtcdDiscovery.h"

#include "Logging.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace {

bool DoRawHttp(const std::string &host, int port, const std::string &method,
               const std::string &path, const std::string &body, std::string *resp) {
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0)
        return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.0\r\nHost: " << host << "\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/x-www-form-urlencoded\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n" << body;
    const std::string raw = req.str();
    if (::send(fd, raw.data(), raw.size(), 0) < 0) {
        ::close(fd);
        return false;
    }
    std::string out;
    char buf[2048];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    if (resp)
        *resp = out;
    return out.find("HTTP/1.") != std::string::npos &&
           (out.find("200") != std::string::npos || out.find("201") != std::string::npos);
}

}  // namespace

EtcdDiscovery &EtcdDiscovery::Instance() {
    static EtcdDiscovery g;
    return g;
}

void EtcdDiscovery::Configure(std::string endpoints, std::string prefix) {
    prefix_ = prefix.empty() ? "gamemesh" : prefix;
    enabled_ = false;
    if (endpoints.empty())
        return;
    // 取第一个 host:port
    std::string e = endpoints;
    const auto comma = e.find(',');
    if (comma != std::string::npos)
        e = e.substr(0, comma);
    if (e.rfind("http://", 0) == 0)
        e = e.substr(7);
    const auto colon = e.find(':');
    if (colon == std::string::npos) {
        host_ = e;
        port_ = 2379;
    } else {
        host_ = e.substr(0, colon);
        port_ = std::atoi(e.substr(colon + 1).c_str());
    }
    enabled_ = !host_.empty();
    if (enabled_)
        LOG_INFO << "EtcdDiscovery configured " << host_ << ":" << port_ << " prefix=" << prefix_;
}

bool EtcdDiscovery::enabled() const { return enabled_; }

bool EtcdDiscovery::HttpPut(const std::string &path, const std::string &body, std::string *resp) {
    return DoRawHttp(host_, port_, "PUT", path, body, resp);
}

bool EtcdDiscovery::HttpGet(const std::string &path, std::string *resp) {
    return DoRawHttp(host_, port_, "GET", path, "", resp);
}

bool EtcdDiscovery::Register(const std::string &service, const std::string &instance_id,
                             const std::string &addr, int ttl_sec) {
    if (!enabled_)
        return false;
    std::ostringstream path;
    path << "/v2/keys/" << prefix_ << "/services/" << service << "/" << instance_id << "?ttl="
         << (ttl_sec > 0 ? ttl_sec : 30);
    const std::string body = "value=" + addr;
    std::string resp;
    const bool ok = HttpPut(path.str(), body, &resp);
    if (!ok)
        LOG_WARN << "EtcdDiscovery Register failed service=" << service << " id=" << instance_id;
    return ok;
}

bool EtcdDiscovery::Discover(const std::string &service, std::vector<std::string> *addrs) {
    if (!enabled_ || !addrs)
        return false;
    addrs->clear();
    std::ostringstream path;
    path << "/v2/keys/" << prefix_ << "/services/" << service << "?recursive=true";
    std::string resp;
    if (!HttpGet(path.str(), &resp))
        return false;
    // 粗糙解析 "value":"host:port"
    size_t pos = 0;
    while ((pos = resp.find("\"value\":\"", pos)) != std::string::npos) {
        pos += 9;
        const size_t end = resp.find('"', pos);
        if (end == std::string::npos)
            break;
        const std::string v = resp.substr(pos, end - pos);
        if (!v.empty() && v.find(':') != std::string::npos)
            addrs->push_back(v);
        pos = end + 1;
    }
    return !addrs->empty();
}
