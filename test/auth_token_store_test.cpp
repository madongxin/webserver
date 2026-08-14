/**
 * 阶段 6：access/refresh 分离；Redis 仅存摘要；refresh 轮换。
 */
#include "AuthTokenStore.h"
#include "PasswordHash.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool RedisReachable() {
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
    RedisClient c;
    return c.Connect(host, port, password);
}

int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

}  // namespace

int main() {
    if (!RedisReachable()) {
        std::printf("FAIL auth_token_store_test (no Redis)\n");
        return 1;
    }
    if (!AuthTokenStore::Instance().InitFromConfig() || !AuthTokenStore::Instance().Available())
        return Fail("init");

    const uint64_t pid = 9006001ULL;
    std::string access, refresh;
    if (!AuthTokenStore::Instance().IssueTokenPair(pid, pid, 120, 600, &access, &refresh))
        return Fail("issue");
    if (access.empty() || refresh.empty() || access == refresh)
        return Fail("tokens must differ");

    std::string err;
    if (!AuthTokenStore::Instance().VerifyAccessToken(pid, access, &err))
        return Fail("verify access");
    if (AuthTokenStore::Instance().VerifyAccessToken(pid, refresh, &err))
        return Fail("refresh must not verify as access");

    // Redis 不得以明文完整 token 为 key
    {
        RedisClient probe;
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
        if (!probe.Connect(host, port, password))
            return Fail("probe redis");
        if (probe.Exists("game:auth:access:" + access))
            return Fail("raw access stored");
        std::string dig;
        if (!PasswordHash::Sha256Hex(access, &dig))
            return Fail("sha");
        if (!probe.Exists("game:auth:access:" + dig))
            return Fail("digest missing");
    }

    std::string neu_a, neu_r;
    if (!AuthTokenStore::Instance().RefreshAccessToken(pid, refresh, 120, &neu_a, &err, &neu_r))
        return Fail("refresh");
    if (neu_a.empty() || neu_r.empty() || neu_a == neu_r)
        return Fail("rotated tokens");
    if (AuthTokenStore::Instance().RefreshAccessToken(pid, refresh, 120, &neu_a, &err, &neu_r))
        return Fail("old refresh must fail");
    if (!AuthTokenStore::Instance().VerifyAccessToken(pid, neu_a, &err))
        return Fail("new access");

    std::printf("PASS auth_token_store_test\n");
    return 0;
}
