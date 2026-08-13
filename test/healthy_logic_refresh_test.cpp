/**
 * GameLogic 健康快照三态：Discover 失败保留、成功替换、成功空列表 fail-closed。
 */
#include "HealthyLogicOwners.h"
#include "IServiceRegistry.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"
#include "RedisServiceRegistry.h"
#include "SessionStore.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool InitRedisPoolFromCnf() {
    const std::string &path = RedisConfigPath::RedisCnf();
    std::ifstream in(path);
    if (!in)
        return false;
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
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
    return RedisPool::Instance().Init(host, port, password, 4);
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!InitRedisPoolFromCnf()) {
        std::printf("FAIL: Redis unavailable\n");
        return 1;
    }
    if (!SessionStore::Instance().InitFromConfig()) {
        std::printf("FAIL: SessionStore init\n");
        return 1;
    }
    const std::string prefix =
        "gamemesh:test:hl:" + std::to_string(static_cast<unsigned long long>(::getpid())) + ":";
    if (!PlacementStore::Instance().InitFromSessionPrefix(prefix, 30)) {
        std::printf("FAIL: PlacementStore init\n");
        return 1;
    }
    RedisServiceRegistry::Get().Configure(prefix);
    if (!RedisServiceRegistry::Get().InitFromRedisConfig()) {
        std::printf("FAIL: RedisServiceRegistry init\n");
        return 1;
    }

    SessionStore::Instance().SetLogicInstanceIds({"gl-static"});
    PlacementStore::Instance().SetLogicOwners({"gl-static"});

    IServiceRegistry::ServiceInstance a;
    a.service = "gamelogic";
    a.instance_id = "gl-hl-0";
    a.address = "127.0.0.1:18211";
    a.port = 18211;
    a.status = "UP";
    Expect(RedisServiceRegistry::Get().RegisterInstance(a, 30), "register gl-hl-0");

    auto applied = RefreshHealthyLogicOwners(false);
    Expect(applied.status == HealthyLogicRefreshStatus::kApplied, "refresh applied");
    Expect(applied.instance_count >= 1, "refresh nonempty");
    {
        const auto ids = SessionStore::Instance().LogicInstanceIds();
        bool found = false;
        for (const auto &id : ids)
            found = found || (id == "gl-hl-0");
        Expect(found, "session owners replaced");
    }

    AcquireSessionInput ain;
    ain.player_id = 910501;
    ain.device_id = "dev-hl";
    ain.server_id = 1;
    ain.kick_other_device = true;
    {
        game::LogoutReq lo;
        lo.set_player_id(ain.player_id);
        game::LogoutRsp lorsp;
        SessionStore::Instance().Logout(lo, &lorsp);
    }
    AcquireSessionResult aout;
    Expect(SessionStore::Instance().AcquireSession(ain, &aout) && aout.ok, "acquire with healthy");
    Expect(aout.gamelogic_instance_id == "gl-hl-0", "acquire routed to discovered");

    const uint64_t fail_before = OpsMetrics::Instance().logic_discover_fail();
    RedisPool::Instance().Shutdown();
    auto failed = RefreshHealthyLogicOwners(false);
    Expect(failed.status == HealthyLogicRefreshStatus::kDiscoverFailed, "discover fail status");
    Expect(OpsMetrics::Instance().logic_discover_fail() > fail_before, "discover fail metric");
    {
        const auto ids = SessionStore::Instance().LogicInstanceIds();
        bool found = false;
        for (const auto &id : ids)
            found = found || (id == "gl-hl-0");
        Expect(found, "snapshot kept on discover fail");
    }
    Expect(InitRedisPoolFromCnf(), "redis reinit");
    Expect(RedisServiceRegistry::Get().InitFromRedisConfig(), "registry reinit");

    Expect(RedisServiceRegistry::Get().UnregisterInstance("gamelogic", "gl-hl-0"), "unreg");
    auto emptied = RefreshHealthyLogicOwners(false);
    Expect(emptied.status == HealthyLogicRefreshStatus::kApplied, "empty applied");
    Expect(emptied.instance_count == 0, "empty count");
    Expect(SessionStore::Instance().LogicInstanceIds().empty(), "session cleared");
    Expect(PlacementStore::Instance().PickHealthyOwner("").empty(), "placement cleared");

    AcquireSessionResult empty_out;
    Expect(!SessionStore::Instance().AcquireSession(ain, &empty_out) && !empty_out.ok,
           "login fail-closed");
    Expect(empty_out.error_code == "NO_HEALTHY_GAMELOGIC", "login error_code");
    Expect(!SessionStore::Instance().IsPlayerOnline(ain.player_id + 1), "no extra session");

    ResolveOrCreateInput pin;
    pin.realm_id = 1;
    pin.map_template_id = 4242;
    ResolveOrCreateResult pout;
    Expect(!PlacementStore::Instance().ResolveOrCreate(pin, &pout), "placement fail-closed");
    Expect(pout.error_code == "NO_HEALTHY_GAMELOGIC", "placement error_code");

    IServiceRegistry::ServiceInstance b = a;
    b.instance_id = "gl-hl-1";
    b.address = "127.0.0.1:18212";
    b.port = 18212;
    Expect(RedisServiceRegistry::Get().RegisterInstance(b, 1), "register ttl=1");
    auto recovered = RefreshHealthyLogicOwners(false);
    Expect(recovered.status == HealthyLogicRefreshStatus::kApplied && recovered.instance_count >= 1,
           "recovered");
    AcquireSessionInput ain2 = ain;
    ain2.player_id = 910502;
    ain2.device_id = "dev-hl-2";
    AcquireSessionResult aout2;
    Expect(SessionStore::Instance().AcquireSession(ain2, &aout2) && aout2.ok, "acquire after recover");

    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto expired = RefreshHealthyLogicOwners(false);
    Expect(expired.status == HealthyLogicRefreshStatus::kApplied, "ttl refresh");
    Expect(expired.instance_count == 0, "ttl expired empty");
    AcquireSessionResult aout3;
    ain2.device_id = "dev-hl-3";
    Expect(!SessionStore::Instance().AcquireSession(ain2, &aout3) || !aout3.ok,
           "ttl expired fail-closed");
    Expect(aout3.error_code == "NO_HEALTHY_GAMELOGIC" || !aout3.ok, "ttl error");

    if (fails) {
        std::printf("healthy_logic_refresh_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK healthy_logic_refresh_test\n");
    return 0;
}
