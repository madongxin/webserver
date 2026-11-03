#include "HttpAdminAccess.h"

#include <cstdio>

static int fails = 0;

static void Expect(bool cond, const char *msg) {
    if (cond)
        return;
    std::printf("FAIL: %s\n", msg);
    ++fails;
}

int main() {
    const auto open = EvaluateHttpAdminAccess(false, "10.0.0.2", "", "");
    Expect(open.allow, "non-formal without token stays open");

    const auto loop = EvaluateHttpAdminAccess(true, "127.0.0.1", "", "");
    Expect(loop.allow, "formal loopback without token is allowed");
    const auto v6 = EvaluateHttpAdminAccess(true, "::1", "", "");
    Expect(v6.allow, "formal ipv6 loopback without token is allowed");

    const auto pub = EvaluateHttpAdminAccess(true, "124.222.244.169", "", "");
    Expect(!pub.allow, "formal public peer without token is refused");

    const auto loop_cfg = EvaluateHttpAdminAccess(true, "127.0.0.1", "secret", "");
    Expect(loop_cfg.allow, "loopback stays open so local scrape and monitor keep working");
    const auto pub_bad = EvaluateHttpAdminAccess(true, "124.222.244.169", "secret", "nope");
    Expect(!pub_bad.allow, "wrong token is refused from a public peer");
    const auto ok = EvaluateHttpAdminAccess(true, "124.222.244.169", "secret", "secret");
    Expect(ok.allow, "matching token allows a public peer");
    Expect(HttpQueryParam("/monitor?access_token=abc&x=1", "access_token") == "abc", "query token");
    Expect(HttpCookieValue("a=b; gamemesh_token=abc", "gamemesh_token") == "abc", "cookie token");

    if (fails) {
        std::printf("http_admin_access_test FAIL %d\n", fails);
        return 1;
    }
    std::printf("PASS http_admin_access_test\n");
    return 0;
}
