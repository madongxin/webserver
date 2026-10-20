#include "GatewayIdentity.h"

#include "FormalMode.h"
#include "Logging.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef WEBSERVER_ENABLE_REDIS
#include "RedisPool.h"
#endif

namespace {

constexpr int kClaimTtlSec = 120;

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

std::string ReadGatewayCnfInstanceId() {
    const char *paths[] = {"../config/gateway.cnf", "config/gateway.cnf", nullptr};
    for (int i = 0; paths[i]; ++i) {
        std::ifstream in(paths[i]);
        if (!in)
            continue;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            if (Trim(line.substr(0, eq)) != "gateway_instance_id")
                continue;
            return Trim(line.substr(eq + 1));
        }
    }
    return {};
}

std::string ReadRedisKeyPrefix() {
    if (const char *env = std::getenv("GAMEMESH_REDIS_PREFIX")) {
        if (*env) {
            std::string p = env;
            if (!p.empty() && p.back() != ':')
                p.push_back(':');
            return p;
        }
    }
    const char *paths[] = {"../config/redis.cnf", "config/redis.cnf", nullptr};
    for (int i = 0; paths[i]; ++i) {
        std::ifstream in(paths[i]);
        if (!in)
            continue;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            if (Trim(line.substr(0, eq)) != "key_prefix")
                continue;
            std::string p = Trim(line.substr(eq + 1));
            if (!p.empty() && p.back() != ':')
                p.push_back(':');
            return p;
        }
    }
    return "gamemesh:dev:";
}

bool LooksLikeListenDerived(const std::string &id) {
    // 禁止历史形态：gw:0.0.0.0:8081 / gw:127.0.0.1:8081
    return id.rfind("gw:", 0) == 0;
}

}  // namespace

GatewayIdentity &GatewayIdentity::Instance() {
    static GatewayIdentity g;
    return g;
}

void GatewayIdentity::Set(const std::string &id) { id_ = id; }

std::string GatewayIdentity::ClaimKey() const {
    return ReadRedisKeyPrefix() + "gw_claim:" + id_;
}

bool GatewayIdentity::Resolve(std::string *err) {
    if (!id_.empty())
        return true;

    if (const char *env = std::getenv("GAMEMESH_INSTANCE_ID")) {
        if (*env)
            id_ = env;
    }
    if (id_.empty())
        id_ = ReadGatewayCnfInstanceId();

    if (id_.empty()) {
        if (FormalModeEnabled()) {
            if (err)
                *err = "gateway_instance_id empty (set GAMEMESH_INSTANCE_ID or gateway.cnf)";
            return false;
        }
        id_ = "gw-0";
        LOG_WARN << "GatewayIdentity: defaulting to gw-0 (non-FORMAL); set GAMEMESH_INSTANCE_ID";
    }

    if (LooksLikeListenDerived(id_)) {
        if (err)
            *err = "gateway_instance_id must not be listen-derived (got " + id_ + ")";
        id_.clear();
        return false;
    }
    if (id_.find("0.0.0.0") != std::string::npos) {
        if (err)
            *err = "gateway_instance_id must not contain 0.0.0.0";
        id_.clear();
        return false;
    }

    LOG_INFO << "GatewayIdentity id=" << id_;
    return true;
}

bool GatewayIdentity::ClaimOrFail(std::string *err) {
    if (id_.empty()) {
        if (err)
            *err = "gateway_instance_id not resolved";
        return false;
    }
#ifdef WEBSERVER_ENABLE_REDIS
    if (!RedisPool::Instance().ready()) {
        LOG_WARN << "GatewayIdentity: Redis unavailable; skip duplicate claim for " << id_;
        return true;
    }
    const std::string key = ClaimKey();
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        if (FormalModeEnabled()) {
            if (err)
                *err = "Redis lease failed for gateway claim";
            return false;
        }
        return true;
    }
    static const char kLuaClaim[] = R"LUA(
local r = redis.call('SET', KEYS[1], ARGV[1], 'NX', 'EX', tonumber(ARGV[2]))
if r then return {'1'} end
return {'0'}
)LUA";
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaClaim, {key}, {id_, std::to_string(kClaimTtlSec)}, &reply) ||
        reply.empty()) {
        if (err)
            *err = "gateway claim Redis error";
        return !FormalModeEnabled();
    }
    if (reply[0] != "1") {
        if (err)
            *err = "duplicate gateway_instance_id already claimed: " + id_;
        return false;
    }
    LOG_INFO << "GatewayIdentity claimed " << key << " ttl_sec=" << kClaimTtlSec;
#else
    (void)err;
#endif
    return true;
}

bool GatewayIdentity::RenewClaim() {
    if (id_.empty())
        return false;
#ifdef WEBSERVER_ENABLE_REDIS
    if (!RedisPool::Instance().ready())
        return false;
    const std::string key = ClaimKey();
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    // 刷新 TTL；仅当 value 仍是本实例时续租，避免覆盖他人 claim。
    static const char kLuaRenew[] = R"LUA(
local cur = redis.call('GET', KEYS[1])
if cur == false then
  local r = redis.call('SET', KEYS[1], ARGV[1], 'NX', 'EX', tonumber(ARGV[2]))
  if r then return {'1', 'RECLAIMED'} end
  return {'0', 'HELD'}
end
if cur ~= ARGV[1] then return {'0', 'FOREIGN'} end
redis.call('EXPIRE', KEYS[1], tonumber(ARGV[2]))
return {'1', 'RENEWED'}
)LUA";
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaRenew, {key}, {id_, std::to_string(kClaimTtlSec)}, &reply) ||
        reply.empty() || reply[0] != "1") {
        LOG_WARN << "GatewayIdentity renew failed key=" << key
                 << " reason=" << (reply.size() > 1 ? reply[1] : "err");
        return false;
    }
    return true;
#else
    return false;
#endif
}
