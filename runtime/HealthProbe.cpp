#include "HealthProbe.h"

#include "FormalMode.h"

#include <atomic>
#include <chrono>

#ifdef WEBSERVER_ENABLE_REDIS
#include "HealthyLogicSnapshot.h"
#include "RedisPool.h"
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
#include "Connection.h"
#include "ConnectionPool.h"
#endif
#ifdef WEBSERVER_ENABLE_BRPC
#include "AuthServiceImpl.h"
#include "BrpcGameDbRepository.h"
#include "GatewayAuthClients.h"
#endif
#ifdef WEBSERVER_ENABLE_REDIS
#include "ServiceHealth.h"
#endif
#ifdef WEBSERVER_ENABLE_BRPC
#include "IServiceRegistry.h"
#endif

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

HealthProbeStore &HealthProbeStore::Instance() {
    static HealthProbeStore g;
    return g;
}

namespace {
std::atomic<bool> g_gw_tcp_listening{false};
}

void HealthProbeStore::SetGatewayTcpListening(bool v) {
    g_gw_tcp_listening.store(v, std::memory_order_release);
}

bool HealthProbeStore::GatewayTcpListening() {
    return g_gw_tcp_listening.load(std::memory_order_acquire);
}

std::shared_ptr<const HealthProbeSnapshot> HealthProbeStore::Current() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
}

bool HealthProbeStore::Fresh(int max_age_ms) const {
    auto s = Current();
    if (!s || s->updated_unix_ms == 0)
        return false;
    return (NowMs() - s->updated_unix_ms) <= max_age_ms;
}

void HealthProbeStore::Refresh(const std::string &role) {
    auto next = std::make_shared<HealthProbeSnapshot>();
    next->updated_unix_ms = NowMs();
    next->ok = true;
    next->detail = "ok";
    auto prev = Current();
    const int prev_fail = prev ? prev->consecutive_failures : 0;
    const int prev_ok = prev ? prev->consecutive_successes : 0;

#ifdef WEBSERVER_ENABLE_REDIS
    const bool need_redis =
        (role == "session" || role == "gateway" || role == "gamelogic" || role == "all");
    if (need_redis) {
        auto lease = RedisPool::Instance().Acquire(500);
        if (!lease || !lease->Ping()) {
            next->ok = false;
            next->detail = "redis ping failed";
        } else if (role == "session" || role == "gateway" || role == "all") {
            auto logic = HealthyLogicSnapshotStore::Instance().Current();
            bool logic_ok = logic && logic->version != 0 &&
                            logic->state != HealthyLogicSnapshot::State::kEmpty &&
                            logic->state != HealthyLogicSnapshot::State::kBootstrap;
#ifdef WEBSERVER_ENABLE_BRPC
            if (!logic_ok && role == "gateway") {
                auto cs = GatewayAuthClients::Instance().CurrentSnapshot();
                logic_ok = cs && !cs->logic.empty();
            }
#endif
            if (!logic_ok) {
                next->ok = false;
                if (!logic || logic->version == 0)
                    next->detail = "logic snapshot missing";
                else if (logic->state == HealthyLogicSnapshot::State::kEmpty)
                    next->detail = "NO_HEALTHY_GAMELOGIC";
                else
                    next->detail = "STARTING";
            }
        }
    }
#endif

#ifdef WEBSERVER_ENABLE_MYSQL
    if (next->ok && (role == "gamedb" || (role == "all" && FormalModeAllowsMysql("all")))) {
        auto *pool = ConnectionPool::getconnectionPool();
        if (!pool || !pool->isInitialized()) {
            next->ok = false;
            next->detail = "mysql pool not initialized";
        } else {
            auto c = pool->getConnection();
            if (!c) {
                next->ok = false;
                next->detail = "mysql acquire failed";
            } else {
                MYSQL_RES *res = c->query("SELECT 1");
                if (!res) {
                    next->ok = false;
                    next->detail = "mysql SELECT 1 failed";
                } else {
                    mysql_free_result(res);
                }
            }
        }
    }
#endif

#ifdef WEBSERVER_ENABLE_BRPC
    if (next->ok && role == "gateway") {
        if (!HealthProbeStore::GatewayTcpListening()) {
            next->ok = false;
            next->detail = "tcp not listening";
        } else if (!GatewayAuthClients::Instance().ready()) {
            next->ok = false;
            next->detail = "session rpc channel not ready";
        }
    }
    if (next->ok && (role == "gamelogic" || role == "world" || role == "all")) {
        if (!BrpcGameDbRepository::Instance().started() ||
            !BrpcGameDbRepository::Instance().Ping(800)) {
            next->ok = false;
            next->detail = "gamedb rpc unreachable";
        }
    }
    if (next->ok && role == "session") {
        if (!AuthGameDbReachable(800)) {
            next->ok = false;
            next->detail = "auth gamedb unreachable";
        }
    }
#endif

    if (next->ok) {
        next->consecutive_successes = prev_ok + 1;
        next->consecutive_failures = 0;
        if (prev && prev->updated_unix_ms != 0 && !prev->ok && next->consecutive_successes < 2) {
            next->ok = false;
            next->detail = std::string("recovering:") + prev->detail;
        }
    } else {
        next->consecutive_failures = prev_fail + 1;
        next->consecutive_successes = 0;
    }

#ifdef WEBSERVER_ENABLE_REDIS
#ifdef WEBSERVER_ENABLE_BRPC
    if (next->consecutive_failures >= 2 && (!prev || prev->ok || prev->consecutive_failures < 2)) {
        const std::string svc = ServiceHealth::Instance().service();
        const std::string sid = ServiceHealth::Instance().server_id();
        if (!svc.empty() && !sid.empty()) {
            ServiceRegistryFacade::Get().Active().SetInstanceStatus(svc, sid, "NOT_READY");
            StaticServiceRegistry::Get().SetInstanceStatus(svc, sid, "NOT_READY");
        }
    }
    if (next->ok && prev && !prev->ok) {
        const std::string svc = ServiceHealth::Instance().service();
        const std::string sid = ServiceHealth::Instance().server_id();
        if (!svc.empty() && !sid.empty()) {
            ServiceRegistryFacade::Get().Active().SetInstanceStatus(svc, sid, "UP");
            StaticServiceRegistry::Get().SetInstanceStatus(svc, sid, "UP");
        }
    }
#endif
#endif

    std::atomic_store_explicit(&snap_, std::shared_ptr<const HealthProbeSnapshot>(next),
                               std::memory_order_release);
}
