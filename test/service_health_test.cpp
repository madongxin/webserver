/**
 * 阶段 7：ServiceHealth live/ready/drain 语义
 */
#include "ServiceHealth.h"

#include <cstdio>
#include <string>

static int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

int main() {
    auto &h = ServiceHealth::Instance();
    h.Configure("gateway", "gw-test");
    h.SetDraining(false);
    h.SetReady(true);
    h.MarkAlive();
    if (!h.ready())
        return Fail("ready");
    if (!h.IsLive(30))
        return Fail("live");
    if (!h.AcceptsNewWork())
        return Fail("accepts");

    h.SetDraining(true);
    if (h.ready())
        return Fail("draining clears ready");
    if (h.AcceptsNewWork())
        return Fail("no new work while draining");

    h.SetDraining(false);
    h.SetReady(true);
    h.MarkAlive();
    const std::string live = h.LivenessJson();
    if (live.find("\"alive\":true") == std::string::npos)
        return Fail("liveness json");
    const std::string ready = h.ReadinessJson(true, "ok");
    if (ready.find("\"ready\":true") == std::string::npos)
        return Fail("readiness json");
    const std::string bad = h.ReadinessJson(false, "redis down");
    if (bad.find("\"ready\":false") == std::string::npos)
        return Fail("readiness deps fail");

    std::printf("PASS service_health_test\n");
    return 0;
}
