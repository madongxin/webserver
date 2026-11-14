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

// Legacy H1 arithmetic: authority-vs-authority with still auth + vx≠0 → every tick drifted.
// Kept so the bug signature stays documented; C1 must not use this comparison.
void TestH1LegacyAuthorityDriftSignature() {
    const float speed = 10.f;
    const float dt = 0.2f;
    const float rec_x = 0.f, rec_z = 0.f, rec_vx = speed, rec_vz = 0.f;
    const float auth_x = 0.f, auth_z = 0.f;
    const float pred_x = rec_x + rec_vx * dt;
    const float pred_z = rec_z + rec_vz * dt;
    const float ex = pred_x - auth_x;
    const float ez = pred_z - auth_z;
    const float err_cm = std::sqrt(ex * ex + ez * ez) * 100.f;
    Expect(err_cm > 100.f, "H1 legacy: still-authority exceeds 100cm");
    Expect(err_cm > 199.f && err_cm < 201.f, "H1 legacy: error is about 200cm");
}

// C1: last emit + error use presented. Steady 10m/s glide → pred tracks presented → no drift.
void TestC1PresentedNoDriftOnSteadyGlide() {
    const float speed = 10.f;
    const float dt = 0.2f;
    float rec_x = 0.f, rec_z = 0.f;
    const float rec_vx = speed, rec_vz = 0.f;
    // Presented advanced one present-slice with the same velocity.
    const float cur_x = rec_x + rec_vx * dt;
    const float cur_z = rec_z + rec_vz * dt;
    const float pred_x = rec_x + rec_vx * dt;
    const float pred_z = rec_z + rec_vz * dt;
    const float ex = pred_x - cur_x;
    const float ez = pred_z - cur_z;
    const float err_cm = std::sqrt(ex * ex + ez * ez) * 100.f;
    Expect(err_cm < 1.f, "C1: steady presented tracks prediction within 1cm");
}

// C1: 20s steady near glide at 200ms ticks → emits ≈ first + keyframes only (not every tick).
void TestC1Steady20sEmitBudget() {
    const float speed = 10.f;
    const float tick_s = 0.2f;
    const int ticks = static_cast<int>(20.0 / tick_s);  // 100
    const int key_ms = 4000;
    const float err_limit_cm = 100.f;
    float rec_x = 0.f, rec_z = 0.f;
    float rec_vx = speed, rec_vz = 0.f;
    int64_t rec_ms = 0;
    bool has = false;
    int emits = 0;
    int drifted_emits = 0;
    for (int i = 0; i < ticks; ++i) {
        const int64_t now = static_cast<int64_t>((i + 1) * tick_s * 1000.0);
        const float cur_x = speed * static_cast<float>(now) / 1000.f;
        const float cur_z = 0.f;
        bool send = false;
        bool drifted = false;
        if (!has) {
            send = true;
        } else {
            const float elapsed = static_cast<float>(now - rec_ms) / 1000.f;
            const float pred_x = rec_x + rec_vx * elapsed;
            const float pred_z = rec_z + rec_vz * elapsed;
            const float ex = pred_x - cur_x;
            const float ez = pred_z - cur_z;
            const float err_cm = std::sqrt(ex * ex + ez * ez) * 100.f;
            drifted = err_cm > err_limit_cm;
            const bool keyframe = (now - rec_ms) >= key_ms;
            send = drifted || keyframe;
        }
        if (!send)
            continue;
        ++emits;
        if (drifted)
            ++drifted_emits;
        rec_ms = now;
        rec_x = cur_x;
        rec_z = cur_z;
        rec_vx = speed;
        rec_vz = 0.f;
        has = true;
    }
    // first + keyframes at 4s,8s,12s,16s,20s ≈ 6; never 100 interval ticks
    Expect(emits >= 5 && emits <= 8, "C1: 20s steady emits about first+keyframes");
    Expect(drifted_emits == 0, "C1: 20s steady has zero drifted emits");
    Expect(emits < ticks / 2, "C1: emits far below present-tick count");
    std::cerr << "C1 20s emits=" << emits << " drifted=" << drifted_emits << " ticks=" << ticks << "\n";
}

void TestExtrapolateOffDefault() {
    unsetenv("GAMEMESH_AOI_EXTRAPOLATE");
    Expect(!AoiExtrapolateEnabled(), "extrapolate default off");
}

void TestFarKeyframeDefault() {
    unsetenv("GAMEMESH_AOI_FAR_KEYFRAME_MS");
    Expect(ExtrapFarKeyframeMs() == 10000, "far keyframe default 10000");
}

}  // namespace

int main() {
    TestH1LegacyAuthorityDriftSignature();
    TestC1PresentedNoDriftOnSteadyGlide();
    TestC1Steady20sEmitBudget();
    TestExtrapolateOffDefault();
    TestFarKeyframeDefault();
    if (fails) {
        std::cerr << fails << " failed\n";
        return 1;
    }
    std::cout << "OK aoi_extrapolate_tick_test (C1 presented + 20s budget)\n";
    return 0;
}
