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
    // 短租约声明；进程退出后 TTL 过期，允许重启同 ID
    const std::string key = "gamemesh:dev:gw_claim:" + id_;
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
    if (!lease->Eval(kLuaClaim, {key}, {id_, "120"}, &reply) || reply.empty()) {
        if (err)
            *err = "gateway claim Redis error";
        return !FormalModeEnabled();
    }
    if (reply[0] != "1") {
        if (err)
            *err = "duplicate gateway_instance_id already claimed: " + id_;
        return false;
    }
    LOG_INFO << "GatewayIdentity claimed " << key;
#else
    (void)err;
#endif
    return true;
}
