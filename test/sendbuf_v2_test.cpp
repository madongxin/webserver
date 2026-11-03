#include "SendBufV2.h"

#include <cstdio>
#include <string>

namespace {
int g_fail = 0;
void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++g_fail;
    }
}
}

int main() {
    SendBufV2 buf;
    int replaced = 0;
    int committed = 0;
    buf.SetCallback([&](std::vector<SendBufV2::Stamp>, bool ok) {
        if (ok)
            ++committed;
        else
            ++replaced;
    });
    buf.PushControl("H1", {});
    buf.SetMove("M1", {{7, 1}});
    buf.SetMove("M2", {{8, 1}});
    const std::string out = buf.TakeAll();
    Expect(out == "H1M2", "control stays ahead of the newest move");
    Expect(replaced == 1, "the replaced move is reported");
    Expect(committed == 1, "the written move is reported");

    SendBufV2 leave;
    int dropped = 0;
    leave.SetCallback([&](std::vector<SendBufV2::Stamp>, bool ok) {
        if (!ok)
            ++dropped;
    });
    leave.SetMove("M7", {{7, 1}});
    leave.PushControl("L7", {7});
    Expect(leave.TakeAll() == "L7", "LEAVE drops a parked position for the same entity");
    Expect(dropped == 1, "dropped position is a replace");

    if (g_fail) {
        std::printf("FAIL sendbuf_v2_test failures=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS sendbuf_v2_test\n");
    return 0;
}
