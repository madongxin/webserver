#include "AoiEvidence.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::cerr << "FAIL: " << m << "\n";
        ++fails;
    }
}

}  // namespace

int main() {
    AoiResetEvidenceForTest();
    AoiNoteTxBytes(AoiWriteFrame::MoveReply, 77, 1);
    AoiNoteTxBytes(AoiWriteFrame::Heartbeat, 40, 1);
    AoiNoteTxBytes(AoiWriteFrame::Reply, 50, 1);
    AoiNoteMoveRequest("ok", 9);
    AoiNoteMoveRequest("stale_seq", 9);
    AoiNoteHeartbeatRequest();
    std::ostringstream os;
    AoiEvidenceWritePrometheus(os);
    const std::string t = os.str();
    Expect(t.find("gamemesh_tx_frames_by_frame{frame=\"move_reply\"} 1") != std::string::npos,
           "move_reply frame counter");
    Expect(t.find("gamemesh_tx_frames_by_frame{frame=\"heartbeat\"} 1") != std::string::npos,
           "heartbeat frame counter");
    Expect(t.find("gamemesh_tx_frames_by_frame{frame=\"reply\"} 1") != std::string::npos,
           "reply stays for non-move control");
    Expect(t.find("gamemesh_move_requests_total{result=\"ok\"} 1") != std::string::npos, "move ok");
    Expect(t.find("gamemesh_move_requests_total{result=\"stale_seq\"} 1") != std::string::npos,
           "move stale");
    Expect(t.find("gamemesh_heartbeat_requests_total 1") != std::string::npos, "heartbeat req");
    Expect(std::strcmp(AoiWriteFrameName(AoiWriteFrame::MoveReply), "move_reply") == 0, "name");
    if (fails) {
        std::cerr << fails << " failed\n";
        return 1;
    }
    std::cout << "OK reply_classify_test\n";
    return 0;
}
