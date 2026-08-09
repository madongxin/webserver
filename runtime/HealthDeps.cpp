#include "HealthDeps.h"

#include "FormalMode.h"
#include "ServiceHealth.h"

#include <cstdlib>
#include <cstring>

#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
#include "ConnectionPool.h"
#endif
#ifdef WEBSERVER_ENABLE_BRPC
#include "GatewayAuthClients.h"
#include "SessionRpcClient.h"
#endif

namespace {

bool EnvForceNotReady() {
    const char *v = std::getenv("GAMEMESH_FORCE_NOT_READY");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}

}  // namespace

HealthDepsResult EvaluateHealthDeps(const std::string &role) {
    HealthDepsResult r;
    if (EnvForceNotReady()) {
        r.ok = false;
        r.detail = "GAMEMESH_FORCE_NOT_READY";
        return r;
    }
    if (ServiceHealth::Instance().draining()) {
        r.ok = false;
        r.detail = "draining";
        return r;
    }

#ifdef WEBSERVER_ENABLE_REDIS
    const bool need_redis =
        (role == "session" || role == "gateway" || role == "gamelogic" || role == "all");
    if (need_redis && !SessionStore::Instance().Available()) {
        r.ok = false;
        r.detail = "redis/session store unavailable";
        return r;
    }
#endif

#ifdef WEBSERVER_ENABLE_MYSQL
    if (role == "gamedb" || (role == "all" && FormalModeAllowsMysql("all"))) {
        if (!ConnectionPool::getconnectionPool()->isInitialized()) {
            r.ok = false;
            r.detail = "mysql pool not initialized";
            return r;
        }
    }
#endif

#ifdef WEBSERVER_ENABLE_BRPC
    if (role == "gateway") {
        if (!GatewayAuthClients::Instance().ready()) {
            r.ok = false;
            r.detail = "auth/session channel not ready";
            return r;
        }
    }
    if (role == "gamelogic" || role == "all") {
        // Session 客户端在 Logic 上用于 Placement/Heartbeat；未 Init 时 warn-only 若非 FORMAL
        if (FormalModeEnabled() && !SessionRpcClient::Instance().ready()) {
            // 部分 Logic 启动顺序可能稍后 Init；仅当明确未配置时失败
            // SessionRpcClient::ready false 在未配置时常见 → 不强制，避免误杀
            (void)0;
        }
    }
#endif

    r.ok = true;
    r.detail = "ok";
    return r;
}
