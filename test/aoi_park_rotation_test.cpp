/**
 * 不依赖 socket。
 * 内核队列长期高于 16KB 时，停放帧每秒最多换一次，不增加失败次数，也不把等待时间清零。
 * 队列放开之后每个实体都要被写出，静默回到 0，而不是整槽停在 in_flight 上按墙钟一直涨。
 */
#include "AoiMoveCoalesceLedger.h"
#include "MoveSendConfig.h"
#include "ParkProbe.h"

#include <cstdio>
#include <vector>

namespace {

int g_fail = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++g_fail;
    }
}

void OfferAll(AoiMoveCoalesceLedger *ledger, int n, int64_t now, uint64_t seq) {
    for (int id = 1; id <= n; ++id)
        ledger->Offer(static_cast<uint64_t>(id), seq, now, 0);
}

std::vector<AoiMoveStamp> PickPark(AoiMoveCoalesceLedger *ledger, int64_t now) {
    int64_t wait = 0;
    auto picked = ledger->Pick(now, 100000, kAoiMoveEventBytes, 6, &wait);
    if (!picked.empty())
        ledger->NoteParked(picked);
    return picked;
}

void TestQueueStuckDoesNotFreezeSlot() {
    constexpr int kN = 40;
    AoiMoveCoalesceLedger frozen;
    OfferAll(&frozen, kN, 1000, 1);
    auto first = PickPark(&frozen, 1000);
    Expect(first.size() == 6, "budget picks six entities");
    Expect(frozen.MaxSilenceMs(11000) >= 10000, "a slot that never writes tracks the wall clock");
    Expect(frozen.UnwrittenAttemptsForTest(1) == 0, "not writing yet does not by itself add attempts");

    AoiMoveCoalesceLedger ledger;
    OfferAll(&ledger, kN, 1000, 1);
    auto parked = PickPark(&ledger, 1000);
    Expect(!parked.empty(), "fixed ledger parks the first frame");
    int64_t last_swap = 1000;
    int swaps = 0;
    for (int64_t t = 1200; t <= 10000; t += 200) {
        OfferAll(&ledger, kN, t, static_cast<uint64_t>(t));
        if (t - last_swap < 1000)
            continue;
        ledger.NoteParkSwapped(parked);
        parked = PickPark(&ledger, t);
        last_swap = t;
        ++swaps;
        Expect(!parked.empty(), "swap still produces a frame");
    }
    Expect(swaps >= 8 && swaps <= 10, "swap happens about once a second");
    Expect(ledger.UnwrittenAttemptsForTest(1) == 0, "swap does not add unwritten_attempts");
    Expect(ledger.PendingSinceForTest(1) == 1000, "swap does not reset pending_since");
    Expect(!parked.empty() && parked[0].state_seq == 10000, "swapped frame keeps the latest coordinate");

    std::vector<int> writes(static_cast<size_t>(kN + 1), 0);
    int64_t t = 10200;
    for (int round = 0; round < kN && writes[0] < kN; ++round) {
        if (parked.empty())
            parked = PickPark(&ledger, t);
        Expect(!parked.empty(), "open queue can pick");
        if (parked.empty())
            break;
        ledger.NoteKernelWrite(parked, t, static_cast<int>(parked.size()) * kAoiMoveEventBytes);
        for (const auto &stamp : parked) {
            if (stamp.entity_id <= static_cast<uint64_t>(kN) &&
                writes[static_cast<size_t>(stamp.entity_id)] == 0) {
                ++writes[static_cast<size_t>(stamp.entity_id)];
                ++writes[0];
            }
        }
        parked.clear();
        t += 200;
    }
    Expect(writes[0] == kN, "every entity is written once the queue accepts");
    Expect(ledger.MaxSilenceMs(t) == 0, "silence drops after the writes, it does not stay on the wall clock");
}

void TestBulkDeferLetsMoveOut() {
    int unsent = 20 * 1024;
    bool wrote = false;
    for (int step = 0; step < 8; ++step) {
        const int bulk = 8000;
        if (!DeferBulkWhileKernelParked(unsent > 16 * 1024, bulk))
            unsent += bulk;
        unsent -= 4000;
        if (unsent < 0)
            unsent = 0;
        if (unsent <= 16 * 1024) {
            wrote = true;
            break;
        }
    }
    Expect(wrote, "deferring bulk frames lets the position frame write within a bound");
    Expect(DeferBulkWhileKernelParked(true, 64) == false, "heartbeat-sized frames are not deferred");
    Expect(DeferBulkWhileKernelParked(false, 8000) == false, "a queue under 16KB does not defer");

    int pinned = 20 * 1024;
    bool pinned_wrote = false;
    for (int step = 0; step < 8; ++step) {
        pinned += 8000;
        pinned -= 4000;
        if (pinned <= 16 * 1024)
            pinned_wrote = true;
    }
    Expect(!pinned_wrote, "without deferral the queue stays above 16KB");
}

void TestParkAgeIgnoresUnrelatedDrain() {
    ParkProbe::EndEpisode(7);
    ParkProbe::BeginFrame(7, 1000);
    ParkProbe::NoteUnrelatedDrain(7);
    Expect(ParkProbe::Parked(7), "unrelated drain keeps the connection parked");
    Expect(ParkProbe::FrameAgeMs(7, 6000) == 5000, "unrelated drain does not clear frame age");
    Expect(ParkProbe::EpisodeAgeMs(7, 6000) == 5000, "unrelated drain does not clear the park episode");
    ParkProbe::EndFrame(7);
    Expect(ParkProbe::FrameAgeMs(7, 6000) == 0, "swap clears the current frame age");
    Expect(ParkProbe::EpisodeAgeMs(7, 6000) == 5000, "swap keeps the episode");
    ParkProbe::BeginFrame(7, 6000);
    Expect(ParkProbe::FrameAgeMs(7, 6500) == 500, "the replacement frame starts its own age");
    Expect(ParkProbe::EpisodeAgeMs(7, 6500) == 5500, "episode continues across the swap");
    ParkProbe::EndEpisode(7);
    Expect(!ParkProbe::Parked(7), "kernel commit ends the park");
    Expect(ParkProbe::FrameAgeMs(7, 7000) == 0, "cleared park does not report a zero as a live sample");
}

}  // namespace

int main() {
    TestQueueStuckDoesNotFreezeSlot();
    TestBulkDeferLetsMoveOut();
    TestParkAgeIgnoresUnrelatedDrain();
    if (g_fail) {
        std::printf("FAIL aoi_park_rotation_test failures=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS aoi_park_rotation_test\n");
    return 0;
}
