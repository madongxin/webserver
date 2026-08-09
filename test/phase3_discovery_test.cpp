/**
 * 阶段三：Redis 动态发现 Register/Discover/DRAINING + 空发现不覆盖
 */
#include "IServiceRegistry.h"
#include "Logging.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"
#include "RedisServiceRegistry.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    {
        std::ifstream in(RedisConfigPath::RedisCnf());
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
                const std::string key = line.substr(0, eq);
                const std::string val = line.substr(eq + 1);
                if (key == "ip")
                    host = val;
                else if (key == "port")
                    port = std::atoi(val.c_str());
                else if (key == "password")
                    password = val;
            }
        }
    }
    if (!RedisPool::Instance().ready() && !RedisPool::Instance().Init(host, port, password, 2)) {
        std::printf("SKIP: Redis unavailable\n");
        return 0;
    }
    RedisServiceRegistry::Get().Configure("gamemesh:p3test:");
    Expect(RedisServiceRegistry::Get().InitFromRedisConfig(), "redis registry init");

    IServiceRegistry::ServiceInstance a;
    a.service = "gamelogic";
    a.instance_id = "gl-p3-a";
    a.address = "127.0.0.1:18201";
    a.port = 18201;
    a.status = "UP";
    Expect(RedisServiceRegistry::Get().RegisterInstance(a, 30), "register a");

    IServiceRegistry::ServiceInstance b;
    b.service = "gamelogic";
    b.instance_id = "gl-p3-b";
    b.address = "127.0.0.1:18202";
    b.port = 18202;
    b.status = "UP";
    Expect(RedisServiceRegistry::Get().RegisterInstance(b, 30), "register b");

    std::vector<std::string> addrs;
    Expect(RedisServiceRegistry::Get().DiscoverAddrs("gamelogic", &addrs) && addrs.size() >= 2,
           "discover two");

    Expect(RedisServiceRegistry::Get().SetInstanceStatus("gamelogic", "gl-p3-a", "DRAINING"),
           "drain a");
    Expect(RedisServiceRegistry::Get().DiscoverAddrs("gamelogic", &addrs) && addrs.size() >= 1,
           "discover after drain");
    for (const auto &x : addrs)
        Expect(x != "127.0.0.1:18201", "draining excluded");

    StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", {"127.0.0.1:8201"}, {"gl-0"});
    StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", {}, {});
    Expect(StaticServiceRegistry::Get().DiscoverAddrs("gamelogic", &addrs) && addrs.size() == 1,
           "empty static keep");

    Expect(RedisServiceRegistry::Get().UnregisterInstance("gamelogic", "gl-p3-a"), "unreg a");
    Expect(RedisServiceRegistry::Get().UnregisterInstance("gamelogic", "gl-p3-b"), "unreg b");

    if (fails) {
        std::printf("phase3_discovery_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK phase3_discovery_test\n");
    return 0;
}
