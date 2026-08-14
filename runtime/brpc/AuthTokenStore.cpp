#include "AuthTokenStore.h"

#include "Logging.h"
#include "PasswordHash.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"
#include "SecureRandom.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>
#include <cstdlib>

namespace {

RedisClient &AuthRedis() {
    static RedisClient c;
    return c;
}

std::string GenHex(size_t n) {
    std::string s;
    if (!SecureRandom::Hex(n, &s))
        return {};
    return s;
}

const char kLuaRotateRefresh[] = R"LUA(
local old_r = KEYS[1]
local new_a = KEYS[2]
local new_r = KEYS[3]
local pkey = KEYS[4]
local pid = ARGV[1]
local aid = ARGV[2]
local a_ttl = tonumber(ARGV[3]) or 3600
local r_ttl = tonumber(ARGV[4]) or 604800
local issued = ARGV[5]
local raw = redis.call('HGETALL', old_r)
if #raw == 0 then return {'TOKEN_ALREADY_ROTATED'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['playerId'] or '') ~= pid then return {'PLAYER_MISMATCH'} end
redis.call('DEL', old_r)
redis.call('HMSET', new_a, 'playerId', pid, 'accountId', aid, 'issuedAt', issued)
redis.call('EXPIRE', new_a, a_ttl)
redis.call('HMSET', new_r, 'playerId', pid, 'accountId', aid, 'issuedAt', issued)
redis.call('EXPIRE', new_r, r_ttl)
redis.call('HMSET', pkey, 'accessDig', new_a, 'refreshDig', new_r)
redis.call('EXPIRE', pkey, r_ttl)
return {'OK'}
)LUA";

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
    if (access.empty() || refresh.empty() || access == refresh)
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

bool AuthTokenStore::RotateRefreshWithClient(RedisClient *c, uint64_t player_id,
                                             const std::string &old_refresh, int access_ttl_sec,
                                             int refresh_ttl_sec, std::string *new_access_out,
                                             std::string *new_refresh_out, std::string *err) {
    if (!c || !new_access_out || player_id == 0 || old_refresh.empty())
        return false;
    std::string old_dig;
    if (!PasswordHash::Sha256Hex(old_refresh, &old_dig)) {
        if (err)
            *err = "digest failed";
        return false;
    }
    const std::string old_key = std::string("game:auth:refresh:") + old_dig;
    std::map<std::string, std::string> fields;
    if (!c->HGetAll(old_key, &fields) || fields.empty()) {
        if (err)
            *err = "TOKEN_ALREADY_ROTATED";
        return false;
    }
    const uint64_t pid =
        static_cast<uint64_t>(std::strtoull(fields["playerId"].c_str(), nullptr, 10));
    if (pid != player_id) {
        if (err)
            *err = "player_id mismatch";
        return false;
    }
    const uint64_t account_id =
        static_cast<uint64_t>(std::strtoull(fields["accountId"].c_str(), nullptr, 10));
    const std::string access = GenHex(32);
    const std::string refresh = GenHex(40);
    if (access.empty() || refresh.empty() || access == refresh) {
        if (err)
            *err = "secure random failed";
        return false;
    }
    std::string a_dig, r_dig;
    if (!PasswordHash::Sha256Hex(access, &a_dig) || !PasswordHash::Sha256Hex(refresh, &r_dig)) {
        if (err)
            *err = "digest failed";
        return false;
    }
    char pbuf[64];
    std::snprintf(pbuf, sizeof(pbuf), "game:auth:player:%llu",
                  static_cast<unsigned long long>(player_id));
    const std::string issued = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::vector<std::string> keys{old_key, std::string("game:auth:access:") + a_dig,
                                  std::string("game:auth:refresh:") + r_dig, pbuf};
    std::vector<std::string> args{std::to_string(player_id),
                                  std::to_string(account_id != 0 ? account_id : player_id),
                                  std::to_string(access_ttl_sec), std::to_string(refresh_ttl_sec),
                                  issued};
    std::vector<std::string> reply;
    if (!c->Eval(kLuaRotateRefresh, keys, args, &reply) || reply.empty()) {
        if (err)
            *err = "lua failed";
        return false;
    }
    if (reply[0] != "OK") {
        if (err)
            *err = reply[0];
        return false;
    }
    *new_access_out = access;
    if (new_refresh_out)
        *new_refresh_out = refresh;
    return true;
}

bool AuthTokenStore::RefreshAccessToken(uint64_t player_id, const std::string &refresh_token,
                                        int access_ttl_sec, std::string *new_access_out,
                                        std::string *err, std::string *new_refresh_out) {
    if (!new_access_out)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!available_) {
        if (err)
            *err = "auth token store unavailable";
        return false;
    }
    if (access_ttl_sec <= 0)
        access_ttl_sec = default_ttl_sec_;
    return RotateRefreshWithClient(&AuthRedis(), player_id, refresh_token, access_ttl_sec,
                                   default_refresh_ttl_sec_, new_access_out, new_refresh_out, err);
}
