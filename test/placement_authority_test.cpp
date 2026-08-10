/**
 * Formal Placement 权威写校验：READY / Owner / epoch / route_version / lease
 */
#include "PlacementAuthority.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

PlacementRecord ReadyAuth() {
    PlacementRecord a;
    a.map_instance_id = 1001;
    a.owner_logic_server_id = "gl-0";
    a.owner_epoch = 7;
    a.route_version = 3;
    a.state = PlacementState::Ready;
    a.lease_until = 2'000'000'000;  // far future
    return a;
}

}  // namespace

int main() {
    const int64_t now = 1'700'000'000;
    std::string code;

    {
        auto a = ReadyAuth();
        Expect(ValidateAuthorityWrite(a, 7, 3, "gl-0", &code, now), "happy path");
    }
    {
        auto a = ReadyAuth();
        a.state = PlacementState::Recovering;
        Expect(!ValidateAuthorityWrite(a, 7, 3, "gl-0", &code, now) &&
                   code == "ERR_PLACEMENT_NOT_READY",
               "recovering rejected");
    }
    {
        auto a = ReadyAuth();
        Expect(!ValidateAuthorityWrite(a, 7, 3, "gl-1", &code, now) &&
                   code == "ERR_WRONG_GAMELOGIC_OWNER",
               "wrong owner");
    }
    {
        auto a = ReadyAuth();
        Expect(!ValidateAuthorityWrite(a, 6, 3, "gl-0", &code, now) && code == "ERR_STALE_EPOCH",
               "stale epoch");
    }
    {
        auto a = ReadyAuth();
        Expect(!ValidateAuthorityWrite(a, 7, 2, "gl-0", &code, now) && code == "ERR_ROUTE_STALE",
               "stale route");
    }
    {
        auto a = ReadyAuth();
        a.lease_until = 0;
        Expect(!ValidateAuthorityWrite(a, 7, 3, "gl-0", &code, now) && code == "ERR_LEASE_MISSING",
               "lease missing");
    }
    {
        auto a = ReadyAuth();
        a.lease_until = now - 1;
        Expect(!ValidateAuthorityWrite(a, 7, 3, "gl-0", &code, now) && code == "ERR_LEASE_EXPIRED",
               "lease expired");
    }
    {
        auto a = ReadyAuth();
        // 非 Formal：req_route_ver == 0 不强制比对
        Expect(ValidateAuthorityWrite(a, 0, 0, "gl-0", &code, now, false),
               "compat zero meta skips epoch/route");
        Expect(!ValidateAuthorityWrite(a, 0, 0, "gl-0", &code, now, true) &&
                   code == "ERR_PLACEMENT_FENCE_REQUIRED",
               "formal rejects zero fence");
    }

    if (fails) {
        std::printf("placement_authority_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK placement_authority_test\n");
    return 0;
}
