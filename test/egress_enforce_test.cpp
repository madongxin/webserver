#include "AoiEvidence.h"
#include "AoiMoveCoalesceLedger.h"
#include "AoiPushSendRoute.h"
#include "MoveSendConfig.h"
#include "ServerStats.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

int fails = 0;

void Expect(bool c, const char *m) {
    if (!c) {
        std::cerr << "FAIL: " << m << "\n";
        ++fails;
    }
}

void ClearEnv() {
    unsetenv("GAMEMESH_AOI_V2_LAYERS");
    unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    unsetenv("GAMEMESH_AOI_V2_HYSTERESIS");
    unsetenv("GAMEMESH_EGRESS_ENFORCE");
    unsetenv("GAMEMESH_EGRESS_SHARED_CLASS_KBIT");
    unsetenv("GAMEMESH_EGRESS_HEADROOM_PERCENT");
    unsetenv("GAMEMESH_EGRESS_MOVE_FLOOR_PERCENT");
    unsetenv("GAMEMESH_EGRESS_RETRANS_RESERVE_PERCENT");
    unsetenv("GAMEMESH_EGRESS_BUDGET_MBPS");
    unsetenv("GAMEMESH_GATEWAY_INSTANCES");
    AoiMoveCoalesceLedger::ResetEgressBudgetCacheForTest();
}

// a: 200 entities, 60s, enforce on → wire <= B*60*1.05
void TestA_BudgetCap() {
    ClearEnv();
    setenv("GAMEMESH_EGRESS_ENFORCE", "1", 1);
    AoiMoveCoalesceLedger ledger;
    // Mirror gateway: enforce only via EGRESS_ENFORCE / AOI_V2_LAYERS, not a bare setter.
    ledger.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    const int B = 2000;
    const int64_t t0 = 100000;
    int64_t wire = 0;
    for (int sec = 0; sec < 60; ++sec) {
        for (int tick = 0; tick < 5; ++tick) {
            const int64_t now = t0 + sec * 1000 + tick * 200;
            for (int id = 1; id <= 200; ++id)
                ledger.Offer(static_cast<uint64_t>(id), static_cast<uint64_t>(sec * 5 + tick + 1), now, 1);
            int64_t wait = 0;
            auto picked = ledger.Pick(now, B, kAoiMoveEventBytes, 200, &wait);
            if (picked.empty())
                continue;
            ledger.NoteParked(picked);
            const int payload = static_cast<int>(picked.size()) * kAoiMoveEventBytes;
            ledger.NoteKernelWrite(picked, now, payload);
            wire += payload + kOnWireHeaderBytes;
        }
    }
    const int64_t cap = static_cast<int64_t>(B) * 60 * 105 / 100;
    Expect(wire <= cap, "a: 200 entities 60s stay within B*60*1.05 on-wire");
    std::cerr << "a wire=" << wire << " cap=" << cap << "\n";
}

// b: control bytes shrink move quota; control not blocked; floor 20%
void TestB_ControlFloor() {
    ClearEnv();
    setenv("GAMEMESH_EGRESS_ENFORCE", "1", 1);
    setenv("GAMEMESH_EGRESS_MOVE_FLOOR_PERCENT", "20", 1);
    AoiMoveCoalesceLedger ledger;
    ledger.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    const int B = 1000;
    const int64_t t0 = 200000;
    // Spend 90% of the window on control; move floor keeps >=20% for positions.
    ledger.NoteControlWire(AoiWriteFrame::Reply, B * 90 / 100 - kOnWireHeaderBytes, t0);
    for (int id = 1; id <= 10; ++id)
        ledger.Offer(static_cast<uint64_t>(id), 1, t0, 1);
    int64_t wait = 0;
    auto picked = ledger.Pick(t0, B, kAoiMoveEventBytes, 40, &wait);
    Expect(!picked.empty(), "b: move still gets a floor slot after heavy control");
    // Control itself is never rejected by the ledger API.
    Expect(ledger.NoteControlWire(AoiWriteFrame::Heartbeat, 40, t0), "b: control note always accepted");
}

// c: oldest silent rotates under budget; hard silence does not break budget
void TestC_RotateNoBreak() {
    ClearEnv();
    setenv("GAMEMESH_EGRESS_ENFORCE", "1", 1);
    AoiMoveCoalesceLedger ledger;
    ledger.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    // Budget fits roughly one event+header per second after 5% retrans reserve.
    const int B = (kAoiMoveEventBytes + kOnWireHeaderBytes) * 100 / 95 + 8;
    const int64_t t0 = 300000;
    for (int id = 1; id <= 5; ++id)
        ledger.Offer(static_cast<uint64_t>(id), 1, t0, 1);
    std::set<uint64_t> seen;
    int64_t wire = 0;
    for (int step = 0; step < 10; ++step) {
        const int64_t now = t0 + step * 1000;
        for (int id = 1; id <= 5; ++id)
            ledger.Offer(static_cast<uint64_t>(id), static_cast<uint64_t>(step + 2), now, 1);
        int64_t wait = 0;
        auto picked = ledger.Pick(now, B, kAoiMoveEventBytes, 40, &wait);
        if (picked.empty())
            continue;
        for (const auto &s : picked)
            seen.insert(s.entity_id);
        ledger.NoteParked(picked);
        const int payload = static_cast<int>(picked.size()) * kAoiMoveEventBytes;
        ledger.NoteKernelWrite(picked, now, payload);
        wire += payload + kOnWireHeaderBytes;
    }
    Expect(seen.size() >= 3, "c: oldest silent entities rotate across seconds");
    Expect(wire <= static_cast<int64_t>(B) * 10 * 105 / 100, "c: hard silence does not break budget");
}

// d: no due-raise past budget when enforce on
void TestD_NoDueRaise() {
    ClearEnv();
    setenv("GAMEMESH_EGRESS_ENFORCE", "1", 1);
    AoiMoveCoalesceLedger with_enforce;
    with_enforce.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    ClearEnv();  // due-raise path: no enforce flags
    AoiMoveCoalesceLedger without;
    without.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    const int B = kOnWireHeaderBytes + kAoiMoveEventBytes;  // one entity
    const int64_t now = 400000;
    for (int id = 1; id <= 20; ++id) {
        with_enforce.Offer(static_cast<uint64_t>(id), 1, now - 500, 1);
        without.Offer(static_cast<uint64_t>(id), 1, now - 500, 1);
    }
    int64_t w1 = 0, w0 = 0;
    auto a = with_enforce.Pick(now, B, kAoiMoveEventBytes, 200, &w1);
    auto b = without.Pick(now, B, kAoiMoveEventBytes, 200, &w0);
    Expect(static_cast<int>(a.size()) <= 1, "d: enforce keeps pick inside one-event budget");
    Expect(static_cast<int>(b.size()) > 1, "d: without enforce due-raise still lifts past budget");
}

// e: two ledgers do not share remaining budget
void TestE_IndependentShares() {
    ClearEnv();
    setenv("GAMEMESH_EGRESS_ENFORCE", "1", 1);
    AoiMoveCoalesceLedger stalled;
    AoiMoveCoalesceLedger live;
    stalled.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    live.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    const int B = 500;
    const int64_t t0 = 500000;
    stalled.Offer(1, 1, t0, 1);
    live.Offer(2, 1, t0, 1);
    int64_t wait = 0;
    auto p1 = stalled.Pick(t0, B, kAoiMoveEventBytes, 10, &wait);
    Expect(!p1.empty(), "e: stalled conn still gets its own share");
    stalled.NoteParked(p1);
    stalled.NoteKernelWrite(p1, t0, kAoiMoveEventBytes);
    // Exhaust stalled window.
    for (int i = 0; i < 20; ++i) {
        stalled.Offer(1, static_cast<uint64_t>(i + 2), t0 + i, 1);
        auto p = stalled.Pick(t0 + i, B, kAoiMoveEventBytes, 10, &wait);
        if (!p.empty()) {
            stalled.NoteParked(p);
            stalled.NoteKernelWrite(p, t0 + i, kAoiMoveEventBytes);
        }
    }
    auto p2 = live.Pick(t0 + 50, B, kAoiMoveEventBytes, 10, &wait);
    Expect(!p2.empty(), "e: live conn does not lose share to stalled conn");
}

// f: retrans reserve shrinks usable move budget
void TestF_RetransReserve() {
    ClearEnv();
    setenv("GAMEMESH_EGRESS_ENFORCE", "1", 1);
    setenv("GAMEMESH_EGRESS_RETRANS_RESERVE_PERCENT", "50", 1);
    AoiMoveCoalesceLedger ledger;
    ledger.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    const int B = 2 * (kAoiMoveEventBytes + kOnWireHeaderBytes);
    const int64_t now = 600000;
    for (int id = 1; id <= 10; ++id)
        ledger.Offer(static_cast<uint64_t>(id), 1, now, 1);
    int64_t wait = 0;
    auto picked = ledger.Pick(now, B, kAoiMoveEventBytes, 40, &wait);
    // 50% reserve → usable ~ one event+header, not two.
    Expect(static_cast<int>(picked.size()) <= 1, "f: 50% retrans reserve halves usable budget");
}

/*
 * g: SHARED_CLASS=2600 kbit, headroom 85%, 2 gateways, 906 conns (~453/gw).
 * Per-gw = 2600*1000/8 * 0.85 / 2 = 138125 B/s.
 * Per-conn = 138125/453 ≈ 305 B/s. After 5% retrans → ≈ 290 B/s.
 * One 1-event MOVE frame on-wire = 52+66 = 118 B → about 2.4 frames/s max.
 * At 5Hz offers with 1 active entity, enforce stretches gaps; require gap <= 3000ms.
 */
void TestG_ScaleInterval() {
    ClearEnv();
    setenv("GAMEMESH_EGRESS_ENFORCE", "1", 1);
    setenv("GAMEMESH_EGRESS_SHARED_CLASS_KBIT", "2600", 1);
    setenv("GAMEMESH_EGRESS_HEADROOM_PERCENT", "85", 1);
    setenv("GAMEMESH_EGRESS_RETRANS_RESERVE_PERCENT", "5", 1);
    setenv("GAMEMESH_GATEWAY_INSTANCES", "2", 1);
    AoiMoveCoalesceLedger::ResetEgressBudgetCacheForTest();
    const int gw = AoiMoveCoalesceLedger::EgressBudgetBytesPerSec();
    Expect(gw == 138125, "g: per-gateway share is 138125 B/s");
    const int per = std::max(1, gw / 453);
    AoiMoveCoalesceLedger ledger;
    ledger.SetEnforceByteBudget(EgressEnforce() || AoiV2Layers());
    const int64_t t0 = 700000;
    int64_t wire = 0;
    int64_t last_write = -1;
    int64_t max_gap = 0;
    const int64_t window_ms = 30000;
    for (int64_t now = t0; now < t0 + window_ms; now += 200) {
        ledger.Offer(1, static_cast<uint64_t>((now - t0) / 200 + 1), now, 1);
        int64_t wait = 0;
        auto picked = ledger.Pick(now, per, kAoiMoveEventBytes, 1, &wait);
        if (picked.empty())
            continue;
        ledger.NoteParked(picked);
        ledger.NoteKernelWrite(picked, now, kAoiMoveEventBytes);
        wire += kAoiMoveEventBytes + kOnWireHeaderBytes;
        if (last_write >= 0) {
            const int64_t gap = now - last_write;
            if (gap > max_gap)
                max_gap = gap;
        }
        last_write = now;
    }
    const int64_t move_cap = static_cast<int64_t>(per) * (window_ms / 1000) * 105 / 100;
    Expect(wire <= move_cap, "g: 30s one-entity wire within per-conn share");
    Expect(last_write >= 0, "g: at least one write");
    Expect(max_gap <= 3000, "g: entity update gap <= 3s");
    std::cerr << "g per=" << per << " wire=" << wire << " cap=" << move_cap << " max_gap=" << max_gap
              << "\n";
}

// h: pure MOVE must use coalesce path; ENTER/LEAVE must not.
void TestH_AoiPushRoute() {
    Expect(AoiDeltaUsesCoalescePath("aoi.delta.v1", true),
           "h: coalescable aoi.delta.v1 uses coalesce/send_frame");
    Expect(!AoiDeltaUsesCoalescePath("aoi.delta.v1", false),
           "h: non-coalescable aoi.delta.v1 stays urgent");
    Expect(!AoiDeltaUsesCoalescePath("item_changed", true),
           "h: non-aoi message never uses AOI coalesce path");
}

}  // namespace

int main() {
    TestA_BudgetCap();
    TestB_ControlFloor();
    TestC_RotateNoBreak();
    TestD_NoDueRaise();
    TestE_IndependentShares();
    TestF_RetransReserve();
    TestG_ScaleInterval();
    TestH_AoiPushRoute();
    ClearEnv();
    if (fails != 0) {
        std::cerr << fails << " failed\n";
        return 1;
    }
    std::cout << "OK egress_enforce_test\n";
    return 0;
}
