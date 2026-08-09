#include "AuthTokenStore.h"

#include "Logging.h"
#include "PasswordHash.h"
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
    std::lock_guard<std::mutex> lk(mu_);
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
            else if (key == "auth_refresh_ttl_sec" && std::atoi(val.c_str()) > 0)
                default_refresh_ttl_sec_ = std::atoi(val.c_str());
        }
    }
    if (!AuthRedis().Connect(host, port, password)) {
        LOG_ERROR << "AuthTokenStore Redis connect failed";
        available_ = false;
        return false;
    }
    available_ = true;
    LOG_INFO << "AuthTokenStore Redis ok access_ttl=" << default_ttl_sec_
             << " refresh_ttl=" << default_refresh_ttl_sec_;
    return true;
}

std::string AuthTokenStore::DigestKey(const char *kind, const std::string &raw_token) const {
    std::string dig;
    if (!PasswordHash::Sha256Hex(raw_token, &dig))
        return {};
    return std::string("game:auth:") + kind + ":" + dig;
}

std::string AuthTokenStore::PlayerKey(uint64_t player_id) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "game:auth:player:%llu",
                  static_cast<unsigned long long>(player_id));
    return buf;
}

bool AuthTokenStore::StoreTokenDigest(const char *kind, const std::string &raw_token,
                                      uint64_t player_id, uint64_t account_id, int ttl_sec) {
    const std::string key = DigestKey(kind, raw_token);
    if (key.empty())
        return false;
    std::map<std::string, std::string> fields;
    fields["playerId"] = std::to_string(player_id);
    fields["accountId"] = std::to_string(account_id);
    fields["issuedAt"] = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (!AuthRedis().HSet(key, fields))
        return false;
    return AuthRedis().Expire(key, ttl_sec);
}

bool AuthTokenStore::VerifyTokenDigest(const char *kind, uint64_t player_id,
                                       const std::string &raw_token, std::string *err,
                                       uint64_t *account_id_out) {
    if (raw_token.empty()) {
        if (err)
            *err = std::string("empty ") + kind + "_token";
        return false;
    }
    const std::string key = DigestKey(kind, raw_token);
    if (key.empty()) {
        if (err)
            *err = "digest failed";
        return false;
    }
    std::map<std::string, std::string> fields;
    if (!AuthRedis().HGetAll(key, &fields) || fields.empty()) {
        if (err)
            *err = std::string("invalid or expired ") + kind + "_token";
        return false;
    }
    const uint64_t pid =
        static_cast<uint64_t>(std::strtoull(fields["playerId"].c_str(), nullptr, 10));
    if (player_id != 0 && pid != player_id) {
        if (err)
            *err = "player_id mismatch";
        return false;
    }
    if (account_id_out)
        *account_id_out =
            static_cast<uint64_t>(std::strtoull(fields["accountId"].c_str(), nullptr, 10));
    return true;
}

bool AuthTokenStore::IssueTokenPair(uint64_t player_id, uint64_t account_id, int access_ttl_sec,
                                    int refresh_ttl_sec, std::string *access_out,
                                    std::string *refresh_out) {
    if (!access_out || !refresh_out || player_id == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!available_)
        return false;
    if (access_ttl_sec <= 0)
        access_ttl_sec = default_ttl_sec_;
    if (refresh_ttl_sec <= 0)
        refresh_ttl_sec = default_refresh_ttl_sec_;
    const std::string access = GenHex(32);
    const std::string refresh = GenHex(40);
    if (access == refresh)
        return false;
    if (!StoreTokenDigest("access", access, player_id, account_id, access_ttl_sec))
        return false;
    if (!StoreTokenDigest("refresh", refresh, player_id, account_id, refresh_ttl_sec)) {
        AuthRedis().Del(DigestKey("access", access));
        return false;
    }
    std::map<std::string, std::string> pfields;
    pfields["accessDig"] = DigestKey("access", access);
    pfields["refreshDig"] = DigestKey("refresh", refresh);
    AuthRedis().HSet(PlayerKey(player_id), pfields);
    AuthRedis().Expire(PlayerKey(player_id), refresh_ttl_sec);
    *access_out = access;
    *refresh_out = refresh;
    return true;
}

bool AuthTokenStore::IssueAccessToken(uint64_t player_id, uint64_t account_id, int ttl_sec,
                                      std::string *access_token_out) {
    std::string refresh;
    return IssueTokenPair(player_id, account_id, ttl_sec, default_refresh_ttl_sec_,
                          access_token_out, &refresh);
}

bool AuthTokenStore::VerifyAccessToken(uint64_t player_id, const std::string &access_token,
                                       std::string *err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!available_) {
        if (err)
            *err = "auth token store unavailable";
        return false;
    }
    return VerifyTokenDigest("access", player_id, access_token, err, nullptr);
}

bool AuthTokenStore::RefreshAccessToken(uint64_t player_id, const std::string &refresh_token,
                                        int access_ttl_sec, std::string *new_access_out,
                                        std::string *err, std::string *new_refresh_out) {
    if (!new_access_out)
        return false;
    uint64_t account_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!available_) {
            if (err)
                *err = "auth token store unavailable";
            return false;
        }
        if (!VerifyTokenDigest("refresh", player_id, refresh_token, err, &account_id))
            return false;
        AuthRedis().Del(DigestKey("refresh", refresh_token));
    }
    std::string neu_refresh;
    if (!IssueTokenPair(player_id, account_id != 0 ? account_id : player_id, access_ttl_sec,
                        default_refresh_ttl_sec_, new_access_out, &neu_refresh)) {
        if (err)
            *err = "reissue failed";
        return false;
    }
    if (new_refresh_out)
        *new_refresh_out = neu_refresh;
    return true;
}
