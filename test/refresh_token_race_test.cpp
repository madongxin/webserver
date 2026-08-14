/**
 * 两个 Redis 连接并发消费同一 refresh token：仅一路成功。
 */
#include "AuthTokenStore.h"
#include "Logging.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool ConnectFromCnf(RedisClient *c) {
    const std::string &path = RedisConfigPath::RedisCnf();
    std::ifstream in(path);
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    if (in) {
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
            if (key == "ip")
                host = val;
            else if (key == "port")
                port = std::atoi(val.c_str());
            else if (key == "password")
                password = val;
        }
    }
    return c->Connect(host, port, password);
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!AuthTokenStore::Instance().InitFromConfig()) {
        std::printf("FAIL auth init\n");
        return 1;
    }
    const uint64_t pid = 9400001ull;
    std::string access, refresh;
    if (!AuthTokenStore::Instance().IssueTokenPair(pid, pid, 120, 600, &access, &refresh)) {
        std::printf("FAIL issue\n");
        return 1;
    }

    RedisClient c1, c2;
    if (!ConnectFromCnf(&c1) || !ConnectFromCnf(&c2)) {
        std::printf("FAIL extra redis clients\n");
        return 1;
    }

    std::atomic<int> ok_n{0};
    std::atomic<int> rotated_n{0};
    std::string a1, r1, a2, r2, e1, e2;
    const int ttl = 120;
    const int rt = AuthTokenStore::Instance().default_refresh_ttl_sec();
    std::thread t1([&]() {
        if (AuthTokenStore::RotateRefreshWithClient(&c1, pid, refresh, ttl, rt, &a1, &r1, &e1))
            ok_n.fetch_add(1);
        else if (e1 == "TOKEN_ALREADY_ROTATED")
            rotated_n.fetch_add(1);
    });
    std::thread t2([&]() {
        if (AuthTokenStore::RotateRefreshWithClient(&c2, pid, refresh, ttl, rt, &a2, &r2, &e2))
            ok_n.fetch_add(1);
        else if (e2 == "TOKEN_ALREADY_ROTATED")
            rotated_n.fetch_add(1);
    });
    t1.join();
    t2.join();
    if (ok_n.load() != 1 || rotated_n.load() != 1) {
        std::printf("FAIL race ok=%d rotated=%d e1=%s e2=%s\n", ok_n.load(), rotated_n.load(),
                    e1.c_str(), e2.c_str());
        return 1;
    }
    const std::string &neu = a1.empty() ? a2 : a1;
    std::string err;
    if (!AuthTokenStore::Instance().VerifyAccessToken(pid, neu, &err)) {
        std::printf("FAIL new access %s\n", err.c_str());
        return 1;
    }
    if (AuthTokenStore::Instance().RefreshAccessToken(pid, refresh, 120, &access, &err, &refresh)) {
        std::printf("FAIL old refresh still works\n");
        return 1;
    }
    std::printf("OK refresh_token_race_test\n");
    return 0;
}
