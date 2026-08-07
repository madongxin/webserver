#include "NatsThinClient.h"

#include "Logging.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

NatsThinClient &NatsThinClient::Instance() {
    static NatsThinClient g;
    return g;
}

void NatsThinClient::SetUrl(std::string url) {
    std::lock_guard<std::mutex> lk(mu_);
    Disconnect();
    url_ = std::move(url);
    host_ = "127.0.0.1";
    port_ = 4222;
    if (url_.empty())
        return;
    // nats://host:port
    std::string u = url_;
    if (u.rfind("nats://", 0) == 0)
        u = u.substr(7);
    const auto colon = u.find(':');
    if (colon == std::string::npos) {
        host_ = u;
    } else {
        host_ = u.substr(0, colon);
        port_ = std::atoi(u.substr(colon + 1).c_str());
    }
}

bool NatsThinClient::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return !url_.empty();
}

void NatsThinClient::Disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool NatsThinClient::EnsureConnected() {
    if (fd_ >= 0)
        return true;
    if (url_.empty())
        return false;
    fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd_ < 0)
        return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        Disconnect();
        return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        Disconnect();
        return false;
    }
    char buf[512];
    const ssize_t n = ::recv(fd_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        Disconnect();
        return false;
    }
    buf[n] = 0;
    const char *connect_msg =
        "CONNECT {\"verbose\":false,\"pedantic\":false,\"lang\":\"cpp\",\"version\":\"0.1\"}\r\n";
    if (::send(fd_, connect_msg, std::strlen(connect_msg), 0) < 0) {
        Disconnect();
        return false;
    }
    return true;
}

bool NatsThinClient::Publish(const std::string &subject, const std::string &payload) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!EnsureConnected())
        return false;
    std::ostringstream os;
    os << "PUB " << subject << " " << payload.size() << "\r\n" << payload << "\r\n";
    const std::string msg = os.str();
    if (::send(fd_, msg.data(), msg.size(), 0) < 0) {
        Disconnect();
        return false;
    }
    return true;
}
