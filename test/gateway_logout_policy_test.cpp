#include "GatewayLogoutPolicy.h"

#include <iostream>

namespace {

int g_fail = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::cerr << "FAIL: " << m << "\n";
        ++g_fail;
    }
}

}  // namespace

int main() {
    gameproto::GatewayLogoutResult r;
    Expect(!gameproto::LogoutAuthoritativeOk(r), "empty not ok");
    Expect(!gameproto::LogoutShouldClearBind(r), "empty must not ForgetBind");

    r.encoded = true;
    r.logic_ok = true;
    r.session_ok = false;
    Expect(!gameproto::LogoutShouldClearBind(r), "session fail keeps bind");
    Expect(!gameproto::LogoutAuthoritativeOk(r), "session fail not authoritative");

    r.logic_ok = false;
    r.session_ok = true;
    Expect(!gameproto::LogoutShouldClearBind(r), "unbind fail keeps bind");

    r.logic_ok = true;
    r.session_ok = true;
    Expect(gameproto::LogoutAuthoritativeOk(r), "both ok");
    Expect(gameproto::LogoutShouldClearBind(r), "both ok clears bind");

    if (g_fail) {
        std::cerr << "gateway_logout_policy_test FAIL count=" << g_fail << "\n";
        return 1;
    }
    std::cout << "gateway_logout_policy_test PASS\n";
    return 0;
}
