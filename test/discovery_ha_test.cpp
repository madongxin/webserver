/**
 * 阶段 3：advertise 校验 / 静态 Registry DRAINING+TTL / brpc list:// naming
 */
#include "AdvertiseAddr.h"
#include "BrpcNamingUtil.h"
#include "IServiceRegistry.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
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
    // advertise 拒绝 0.0.0.0
    {
        std::string err;
        Expect(!ValidateAdvertiseAddr("0.0.0.0:8401", &err), "reject 0.0.0.0 advertise");
        Expect(ValidateAdvertiseAddr("127.0.0.1:8401", &err), "accept 127.0.0.1");
        Expect(IsForbiddenAdvertiseHost("0.0.0.0"), "forbidden host");
        Expect(AdvertiseFromListen("0.0.0.0:8401").find(":8401") != std::string::npos,
               "advertise from listen keeps port");
        Expect(PortFromHostPort("127.0.0.1:8402") == 8402, "port parse");
    }

    // list:// naming
    {
        Expect(BuildListNamingUrl({"127.0.0.1:8401"}) == "127.0.0.1:8401", "single naming");
        Expect(BuildListNamingUrl({"127.0.0.1:8401", "127.0.0.1:8402"}) ==
                   "list://127.0.0.1:8401,127.0.0.1:8402",
               "multi list://");
        Expect(SessionLoadBalancerName(1).empty(), "single no lb");
        Expect(SessionLoadBalancerName(2) == "rr", "multi rr");
    }

    // 空 SetStatic 不覆盖；DRAINING 不进 Discover
    {
        StaticServiceRegistry::Get().SetStaticAddrs("session", {"127.0.0.1:8401", "127.0.0.1:8402"},
                                                    {"sess-0", "sess-1"});
        StaticServiceRegistry::Get().SetStaticAddrs("session", {}, {});
        std::vector<std::string> addrs;
        Expect(StaticServiceRegistry::Get().DiscoverAddrs("session", &addrs) && addrs.size() == 2,
               "empty SetStatic keeps peers");
        Expect(StaticServiceRegistry::Get().SetInstanceStatus("session", "sess-0", "DRAINING"),
               "set draining");
        Expect(StaticServiceRegistry::Get().DiscoverAddrs("session", &addrs) && addrs.size() == 1 &&
                   addrs[0] == "127.0.0.1:8402",
               "DRAINING excluded from Discover");
        std::vector<IServiceRegistry::ServiceInstance> all;
        Expect(StaticServiceRegistry::Get().DiscoverAll("session", &all) && all.size() == 2,
               "DiscoverAll includes DRAINING");
        Expect(StaticServiceRegistry::Get().UnregisterInstance("session", "sess-0"), "unregister");
        Expect(StaticServiceRegistry::Get().DiscoverAddrs("session", &addrs) && addrs.size() == 1,
               "after unregister one left");
    }

    // TTL 过期后消失；Renew 可续命
    {
        IServiceRegistry::ServiceInstance inst;
        inst.service = "probe";
        inst.instance_id = "p-ttl";
        inst.address = "127.0.0.1:19999";
        inst.status = "UP";
        Expect(StaticServiceRegistry::Get().RegisterInstance(inst, 1), "register ttl=1s");
        std::vector<std::string> addrs;
        Expect(StaticServiceRegistry::Get().DiscoverAddrs("probe", &addrs) && addrs.size() == 1,
               "visible before expire");
        Expect(StaticServiceRegistry::Get().RenewInstance("probe", "p-ttl", 3), "renew");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        Expect(StaticServiceRegistry::Get().DiscoverAddrs("probe", &addrs) && addrs.size() == 1,
               "still alive after renew beyond first ttl");
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        Expect(!StaticServiceRegistry::Get().DiscoverAddrs("probe", &addrs),
               "expired after renewed ttl");
    }

    if (fails) {
        std::printf("%d failures\n", fails);
        return 1;
    }
    std::printf("discovery_ha_test passed\n");
    return 0;
}
