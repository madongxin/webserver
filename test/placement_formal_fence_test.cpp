/**
 * 阶段一：Formal Placement fence 必须完整（epoch/route 不得为 0）
 */
#include "PlacementAuthority.h"

#include <cstdio>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
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
    a.lease_until = 2'000'000'000;
    return a;
}

}  // namespace

int main() {
    const int64_t now = 1'700'000'000;
    std::string code;
    auto a = ReadyAuth();

    Expect(!ValidateAuthorityWrite(a, 0, 3, "gl-0", &code, now, true) &&
               code == "ERR_PLACEMENT_FENCE_REQUIRED",
           "formal zero epoch");
    Expect(!ValidateAuthorityWrite(a, 7, 0, "gl-0", &code, now, true) &&
               code == "ERR_PLACEMENT_FENCE_REQUIRED",
           "formal zero route");
    Expect(ValidateAuthorityWrite(a, 7, 3, "gl-0", &code, now, true), "formal happy");
    Expect(!ValidateAuthorityWrite(a, 6, 3, "gl-0", &code, now, true) && code == "ERR_STALE_EPOCH",
           "formal stale epoch");
    // 非 Formal：0 仍可跳过
    Expect(ValidateAuthorityWrite(a, 0, 0, "gl-0", &code, now, false), "compat zero skip");

    if (fails) {
        std::printf("placement_formal_fence_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK placement_formal_fence_test\n");
    return 0;
}
