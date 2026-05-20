#include "PrometheusClient.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <sstream>
#include <vector>

namespace {

constexpr const char *kHost = "127.0.0.1";
constexpr int kPort = 9090;

std::string UrlEncode(const std::string &s) {
    std::ostringstream os;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            os << c;
        else
            os << '%' << std::hex << std::uppercase << (c >> 4) << (c & 0xF) << std::nouppercase
               << std::dec;
    }
    return os.str();
}

bool SendAll(int fd, const std::string &data) {
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(data.size())) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - static_cast<size_t>(sent), 0);
        if (n <= 0)
            return false;
        sent += n;
    }
    return true;
}

bool RecvHttp(int fd, int *status, std::string *body, std::string *err) {
    std::string raw;
    char buf[8192];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (err)
                *err = "recv failed";
            return false;
        }
        raw.append(buf, static_cast<size_t>(n));
        if (raw.size() > 8 * 1024 * 1024) {
            if (err)
                *err = "response too large";
            return false;
        }
    }
    const size_t hdr_end = raw.find("\r\n\r\n");
    const std::string hdr = raw.substr(0, hdr_end);
    std::string entity = raw.substr(hdr_end + 4);
    *status = 0;
    if (hdr.size() >= 12 && hdr.compare(0, 4, "HTTP") == 0)
        *status = std::atoi(hdr.c_str() + 9);

    bool chunked = (hdr.find("Transfer-Encoding: chunked") != std::string::npos ||
                    hdr.find("transfer-encoding: chunked") != std::string::npos);
    int content_len = -1;
    {
        const auto p = hdr.find("Content-Length:");
        if (p == std::string::npos) {
            const auto p2 = hdr.find("content-length:");
            if (p2 != std::string::npos)
                content_len = std::atoi(hdr.c_str() + p2 + 15);
        } else {
            content_len = std::atoi(hdr.c_str() + p + 15);
        }
    }

    if (chunked) {
        std::string decoded;
        while (true) {
            while (entity.find("\r\n") == std::string::npos) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                entity.append(buf, static_cast<size_t>(n));
            }
            const size_t line_end = entity.find("\r\n");
            if (line_end == std::string::npos)
                break;
            const unsigned long chunk_sz = std::strtoul(entity.c_str(), nullptr, 16);
            entity.erase(0, line_end + 2);
            if (chunk_sz == 0)
                break;
            while (entity.size() < chunk_sz + 2) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                entity.append(buf, static_cast<size_t>(n));
            }
            decoded.append(entity.data(), chunk_sz);
            entity.erase(0, chunk_sz + 2);
        }
        *body = decoded;
        return true;
    }

    while (content_len < 0 || static_cast<int>(entity.size()) < content_len) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        entity.append(buf, static_cast<size_t>(n));
    }
    if (content_len >= 0 && static_cast<int>(entity.size()) > content_len)
        entity.resize(static_cast<size_t>(content_len));
    *body = entity;
    return true;
}

PrometheusClientResult QueryPath(const std::string &path) {
    PrometheusClientResult r;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        r.error = "socket failed";
        return r;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(kPort));
    ::inet_pton(AF_INET, kHost, &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        r.error = "connect prometheus failed";
        ::close(fd);
        return r;
    }
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\nHost: " << kHost << "\r\nConnection: close\r\n\r\n";
    if (!SendAll(fd, req.str())) {
        r.error = "send failed";
        ::close(fd);
        return r;
    }
    if (!RecvHttp(fd, &r.http_status, &r.body, &r.error)) {
        ::close(fd);
        return r;
    }
    ::close(fd);
    return r;
}

}  // namespace

PrometheusClientResult PrometheusInstantQuery(const std::string &query) {
    return QueryPath("/api/v1/query?query=" + UrlEncode(query));
}

PrometheusClientResult PrometheusQueryRange(const std::string &query, const std::string &start,
                                            const std::string &end, const std::string &step) {
    return QueryPath("/api/v1/query_range?query=" + UrlEncode(query) + "&start=" + UrlEncode(start) +
                     "&end=" + UrlEncode(end) + "&step=" + UrlEncode(step));
}
