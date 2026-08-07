/**
 * 游戏 TCP 冒烟：Login，或 Register→Login。
 * 用法:
 *   game_tcp_smoke_client <host> <port> <player_id>
 *   game_tcp_smoke_client <host> <port> register [device_id]
 */
#include "ProtoFraming.h"
#include "game.pb.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool SendAll(int fd, const char *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, data + off, n - off);
        if (w <= 0)
            return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

bool RecvAll(int fd, char *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = ::read(fd, data + off, n - off);
        if (r <= 0)
            return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

bool Exchange(int fd, const game::GameRequest &req, game::GameResponse *rsp) {
    std::string body;
    if (!req.SerializeToString(&body))
        return false;
    std::string frame;
    if (!gameproto::EncodeFrame(body, &frame))
        return false;
    if (!SendAll(fd, frame.data(), frame.size()))
        return false;
    uint32_t be_len = 0;
    if (!RecvAll(fd, reinterpret_cast<char *>(&be_len), 4))
        return false;
    const uint32_t len = ntohl(be_len);
    if (len == 0 || len > 4 * 1024 * 1024)
        return false;
    std::string rsp_body(len, '\0');
    if (!RecvAll(fd, &rsp_body[0], len))
        return false;
    return rsp->ParseFromString(rsp_body);
}

int Connect(const char *host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::printf("usage: %s <host> <port> <player_id>\n", argv[0]);
        std::printf("       %s <host> <port> register [device_id]\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    const int port = std::atoi(argv[2]);
    const bool do_register = (std::strcmp(argv[3], "register") == 0);

    int fd = Connect(host, port);
    if (fd < 0) {
        std::perror("connect");
        return 6;
    }

    uint64_t player_id = 0;
    if (do_register) {
        game::GameRequest req;
        req.set_seq(1);
        auto *reg = req.mutable_register_();
        reg->set_device_id(argc >= 5 ? argv[4] : "smoke-device");
        reg->set_display_name("smoke");
        game::GameResponse rsp;
        if (!Exchange(fd, req, &rsp) || !rsp.ok() || !rsp.has_register_() || !rsp.register_().ok()) {
            std::printf("register fail ok=%d msg=%s\n", rsp.ok() ? 1 : 0, rsp.message().c_str());
            ::close(fd);
            return 12;
        }
        player_id = rsp.register_().player_id();
        std::printf("register.ok=1 player_id=%llu\n",
                    static_cast<unsigned long long>(player_id));
    } else {
        player_id = static_cast<uint64_t>(std::strtoull(argv[3], nullptr, 10));
    }

    game::GameRequest req;
    req.set_seq(2);
    auto *login = req.mutable_login();
    login->set_player_id(player_id);
    login->set_device_id("smoke");
    login->set_server_id(1);
    game::GameResponse rsp;
    if (!Exchange(fd, req, &rsp)) {
        std::printf("login exchange fail\n");
        ::close(fd);
        return 10;
    }
    ::close(fd);

    std::printf("ok=%d msg=%s\n", rsp.ok() ? 1 : 0, rsp.message().c_str());
    if (rsp.has_login()) {
        std::printf("login.ok=%d token_len=%zu server_id=%u player_id=%llu\n",
                    rsp.login().ok() ? 1 : 0, rsp.login().token().size(), rsp.login().server_id(),
                    static_cast<unsigned long long>(player_id));
    }
    return rsp.ok() ? 0 : 12;
}
