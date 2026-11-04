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

    struct Shadow {
        std::vector<std::string> control;
        std::string move;
        uint64_t move_id = 0;
        std::string Take() {
            std::string out;
            for (const auto &frame : control)
                out.append(frame);
            control.clear();
            out.append(move);
            move.clear();
            move_id = 0;
            return out;
        }
        void Push(const std::string &frame, uint64_t strip) {
            if (strip != 0 && move_id == strip)
                move.clear();
            control.push_back(frame);
        }
        void Move(const std::string &frame, uint64_t id) {
            move = frame;
            move_id = id;
        }
    };
    Shadow shadow;
    SendBufV2 diff;
    unsigned seed = 1;
    for (int i = 0; i < 64; ++i) {
        seed = seed * 1103515245u + 12345u;
        const int op = static_cast<int>((seed >> 16) % 3);
        if (op == 0) {
            const std::string frame = "C" + std::to_string(i);
            const uint64_t strip = (seed % 5 == 0) ? 7 : 0;
            shadow.Push(frame, strip);
            diff.PushControl(frame, strip ? std::vector<uint64_t>{strip} : std::vector<uint64_t>{});
        } else if (op == 1) {
            const std::string frame = "M" + std::to_string(i);
            shadow.Move(frame, 7);
            diff.SetMove(frame, {{7, static_cast<uint64_t>(i)}});
        } else {
            Expect(shadow.Take() == diff.TakeAll(), "random operations match the old buffer order");
        }
    }
    Expect(shadow.Take() == diff.TakeAll(), "the tail of the random stream matches");

    SendBufV2 golden;
    golden.PushControl("H1", {});
    golden.SetMove("M1", {{1, 1}});
    golden.PushControl("H2", {});
    golden.SetMove("M2", {{1, 2}});
    Expect(golden.TakeAll() == "H1H2M2", "golden stream keeps control order and the newest move");

    if (g_fail) {
        std::printf("FAIL sendbuf_v2_test failures=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS sendbuf_v2_test\n");
    return 0;
}
