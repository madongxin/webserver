#include "AuthTokenStore.h"

#include "Logging.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>

namespace {

RedisClient &AuthRedis() {
    static RedisClient c;
    return c;
}

std::string GenHex(size_t n) {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    static const char hex[] = "0123456789abcdef";
    std::string s(n, '0');
    for (char &c : s)
        c = hex[gen() & 0xf];
    return s;
}

}  // namespace

AuthTokenStore &AuthTokenStore::Instance() {
    static AuthTokenStore g;
    return g;
}

bool AuthTokenStore::InitFromConfig() {
    if (available_)
        return true;
    const std::string &path = RedisConfigPath::RedisCnf();
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    std::ifstream in(path);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                while (!s.empty() && s.front() == ' ')
                    s.erase(s.begin());
                while (!s.empty() && s.back() == ' ')
                    s.pop_back();
                return s;
            };
            const std::string key = trim(line.substr(0, eq));
            const std::string val = trim(line.substr(eq + 1));
            if (key == "ip")
                host = val;
            else if (key == "port")
                port = std::atoi(val.c_str());
            else if (key == "password")
                password = val;
            else if (key == "auth_token_ttl_sec" && std::atoi(val.c_str()) > 0)
                default_ttl_sec_ = std::atoi(val.c_str());
        }
    }
    if (!AuthRedis().Connect(host, port, password)) {
        LOG_ERROR << "AuthTokenStore Redis connect failed";
        available_ = false;
        return false;
    }
    available_ = true;
    LOG_INFO << "AuthTokenStore Redis ok ttl=" << default_ttl_sec_;
    return true;
}

std::string AuthTokenStore::TokenKey(const std::string &token) const {
    return "game:auth:access:" + token;
}

std::string AuthTokenStore::PlayerKey(uint64_t player_id) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "game:auth:player:%llu",
                  static_cast<unsigned long long>(player_id));
    return buf;
}

bool AuthTokenStore::IssueAccessToken(uint64_t player_id, uint64_t account_id, int ttl_sec,
                                      std::string *access_token_out) {
    if (!available_ || !access_token_out || player_id == 0)
        return false;
    if (ttl_sec <= 0)
        ttl_sec = default_ttl_sec_;
    const std::string token = GenHex(32);
    std::map<std::string, std::string> fields;
    fields["playerId"] = std::to_string(player_id);
    fields["accountId"] = std::to_string(account_id);
    fields["issuedAt"] = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (!AuthRedis().HSet(TokenKey(token), fields))
        return false;
    if (!AuthRedis().Expire(TokenKey(token), ttl_sec))
        return false;
    // 玩家当前 access token（便于吊销旧票）
    std::map<std::string, std::string> pfields{{"token", token}};
    AuthRedis().HSet(PlayerKey(player_id), pfields);
    AuthRedis().Expire(PlayerKey(player_id), ttl_sec);
    *access_token_out = token;
    return true;
}

bool AuthTokenStore::VerifyAccessToken(uint64_t player_id, const std::string &access_token,
                                       std::string *err) {
    if (!available_) {
        if (err)
            *err = "auth token store unavailable";
        return false;
    }
    if (access_token.empty()) {
        if (err)
            *err = "empty access_token";
        return false;
    }
    std::map<std::string, std::string> fields;
    if (!AuthRedis().HGetAll(TokenKey(access_token), &fields) || fields.empty()) {
        if (err)
            *err = "invalid or expired access_token";
        return false;
    }
    const uint64_t pid =
        static_cast<uint64_t>(std::strtoull(fields["playerId"].c_str(), nullptr, 10));
    if (player_id != 0 && pid != player_id) {
        if (err)
            *err = "player_id mismatch";
        return false;
    }
    return true;
}

bool AuthTokenStore::RefreshAccessToken(uint64_t player_id, const std::string &old_token,
                                        int ttl_sec, std::string *new_token_out, std::string *err) {
    if (!VerifyAccessToken(player_id, old_token, err))
        return false;
    AuthRedis().Del(TokenKey(old_token));
    return IssueAccessToken(player_id, player_id, ttl_sec, new_token_out);
}
