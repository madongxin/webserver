#include "MoveSendConfig.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::cerr << "FAIL: " << m << "\n";
        ++fails;
    }
}

// H1: AllowMoveAoi stores rec at authoritative subject.x/z, predicts with rec.v, compares to
// subject.x/z again. Presented aoi_* is what EmitTo sends but is not in the error term.
// When authority sits still for 200ms while vx=10 (e.g. arrived / sparse segment), 
// pred advances 2m → err 200cm > ExtrapErrorCm 100 → drifted every present tick.
void TestH1DriftUsesAuthority() {
    const float speed = 10.f;
    const float dt = 0.2f;
    const float rec_x = 0.f, rec_z = 0.f, rec_vx = speed, rec_vz = 0.f;
    const float auth_x = 0.f, auth_z = 0.f;  // authority unchanged
    const float pred_x = rec_x + rec_vx * dt;
    const float pred_z = rec_z + rec_vz * dt;
    const float ex = pred_x - auth_x;
    const float ez = pred_z - auth_z;
    const float err_cm = std::sqrt(ex * ex + ez * ez) * 100.f;
    Expect(err_cm > 100.f, "H1: 200ms@10m/s with still authority exceeds 100cm");
    Expect(err_cm > 199.f && err_cm < 201.f, "H1: error is about 200cm");
}

void TestExtrapolateOffDefault() {
    unsetenv("GAMEMESH_AOI_EXTRAPOLATE");
    Expect(!AoiExtrapolateEnabled(), "extrapolate default off");
}

}  // namespace

int main() {
    TestH1DriftUsesAuthority();
    TestExtrapolateOffDefault();
    if (fails) {
        std::cerr << fails << " failed\n";
        return 1;
    }
    std::cout << "OK aoi_extrapolate_tick_test (H1 arithmetic; full AdvancePresented tick counts deferred to C1)\n";
    return 0;
}
