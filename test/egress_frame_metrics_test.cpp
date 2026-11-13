#include "AoiEvidence.h"

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

bool Has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

}  // namespace

int main() {
    AoiResetEvidenceForTest();
    {
        std::ostringstream empty;
        AoiEvidenceWritePrometheus(empty);
        const std::string text = empty.str();
        Expect(!Has(text, "gamemesh_move_frame_events{quantile="), "empty event quantile omitted");
        Expect(!Has(text, "gamemesh_move_frame_events_count"), "empty event count omitted");
        Expect(!Has(text, "gamemesh_move_frame_payload_bytes{quantile="), "empty payload quantile omitted");
        Expect(Has(text, "gamemesh_tx_frames_by_frame{frame=\"move\"} 0"), "move frame counter stays present");
    }
    AoiNoteMoveFrameSample(1, 80);
    AoiNoteMoveFrameSample(1, 82);
    AoiNoteMoveFrameSample(4, 200);
    std::ostringstream os;
    AoiEvidenceWritePrometheus(os);
    const std::string text = os.str();
    Expect(Has(text, "gamemesh_move_frame_events{quantile=\"0.5\"} 1"), "event p50");
    Expect(Has(text, "gamemesh_move_frame_events_count 3"), "event sample count");
    Expect(Has(text, "gamemesh_move_frame_payload_bytes{quantile=\"0.5\"} 82"), "payload p50");
    Expect(Has(text, "gamemesh_move_frame_payload_bytes_count 3"), "payload sample count");
    Expect(Has(text, "gamemesh_tx_frames_by_frame{frame=\"move\"} 0"),
           "frame sample does not invent tx frames");
    if (fails != 0) {
        std::cerr << fails << " failed\n";
        return 1;
    }
    std::cout << "OK egress_frame_metrics_test\n";
    return 0;
}
