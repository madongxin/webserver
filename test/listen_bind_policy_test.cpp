#include "BindAddressPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_fails = 0;

void Expect(bool cond, const char *msg) {
    if (cond) {
        std::printf("ok: %s\n", msg);
        return;
    }
    std::printf("FAIL: %s\n", msg);
    ++g_fails;
}

}  // namespace

int main() {
    Expect(EvaluateListenBind("127.0.0.1:8201", true, false).ok, "formal loopback");
    Expect(EvaluateListenBind("10.0.0.2:8181", true, false).ok, "formal private");
    Expect(EvaluateListenBind("192.168.1.9:8501", true, false).ok, "formal rfc1918");
    Expect(!EvaluateListenBind("0.0.0.0:8201", true, false).ok, "formal wildcard refused");
    Expect(!EvaluateListenBind("1.2.3.4:8201", true, false).ok, "formal public refused");
    Expect(!EvaluateListenBind("::", true, false).ok, "formal ipv6 wildcard refused");
    const ListenBindDecision denied = EvaluateListenBind("0.0.0.0:8181", true, false);
    Expect(denied.error.find("allow_public_bind=true") != std::string::npos, "error names the switch");
    Expect(EvaluateListenBind("0.0.0.0:8181", true, true).ok, "explicit allow_public_bind");
    Expect(EvaluateListenBind("1.2.3.4:8201", false, false).ok, "dev mode may bind public");
    Expect(!EvaluateListenBind("", true, true).ok, "empty address refused");
    Expect(!AllowPublicBindFromEnv() || std::getenv("GAMEMESH_ALLOW_PUBLIC_BIND") != nullptr,
           "env switch is opt-in");
    if (g_fails) {
        std::printf("%d failures\n", g_fails);
        return 1;
    }
    std::printf("listen_bind_policy_test PASS\n");
    return 0;
}
