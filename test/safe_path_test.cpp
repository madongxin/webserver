#include "SafePath.h"

#include <cstdio>
#include <string>

static int g_fails = 0;

static void Expect(bool ok, const char *msg) {
    if (ok) {
        std::printf("ok: %s\n", msg);
        return;
    }
    std::printf("FAIL: %s\n", msg);
    ++g_fails;
}

int main() {
    std::string out;
    Expect(!SafeJoinUnderRoot("/var/gamemesh/files", "../../etc/passwd", &out),
           "reject unix dotdot");
    Expect(!SafeJoinUnderRoot("/var/gamemesh/files", "..\\..\\windows\\system32\\config", &out),
           "reject windows dotdot");
    Expect(!SafeJoinUnderRoot("/var/gamemesh/files", "/etc/passwd", &out), "reject absolute");
    Expect(SafeJoinUnderRoot("/var/gamemesh/files", "readme.txt", &out) &&
               out == "/var/gamemesh/files/readme.txt",
           "plain name stays inside root");
    std::string base;
    Expect(SafeBasename("../../etc/passwd", &base) && base.find("..") == std::string::npos &&
               base.find("passwd") != std::string::npos,
           "upload basename drops directories");
    Expect(!SafeBasename("..", &base), "bare dotdot rejected");
    Expect(SafeBasename("shot.png", &base), "plain upload name accepted");
    if (g_fails) {
        std::printf("%d failures\n", g_fails);
        return 1;
    }
    std::printf("safe_path_test PASS\n");
    return 0;
}
