/**
 * 不依赖 socket。复现「内核一直塞着，停放帧每 1 秒才写出一次」时，
 * 旧账本只会反复写出同一批实体；修复后的账本轮转，静默有上限。
 */
#include "AoiMoveCoalesceLedger.h"

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

struct RunResult {
    int unwritten = 0;
    int64_t max_silence_ms = 0;
    int distinct_written = 0;
};

RunResult Simulate(bool legacy, bool intermittent) {
    constexpr int kEntities = 200;
    constexpr int kPerFrame = 40;
    constexpr int64_t kStep = 200;
    constexpr int64_t kWriteEvery = 1000;
    constexpr int kEst = 96;
    AoiMoveCoalesceLedger ledger;
    ledger.EnableLegacyAccountingForTest(legacy);
    std::vector<AoiMoveStamp> parked;
    int64_t parked_at = -1;
    std::vector<int> writes(static_cast<size_t>(kEntities + 1), 0);
    int64_t max_silence = 0;

    auto commit = [&](int64_t now) {
        ledger.NoteKernelWrite(parked, now, static_cast<int>(parked.size()) * kEst);
        for (const auto &stamp : parked) {
            if (stamp.entity_id <= static_cast<uint64_t>(kEntities))
                ++writes[static_cast<size_t>(stamp.entity_id)];
        }
        parked.clear();
        parked_at = -1;
    };

    for (int64_t t = kStep; t <= 40000; t += kStep) {
        for (int id = 1; id <= kEntities; ++id)
            ledger.Offer(static_cast<uint64_t>(id), static_cast<uint64_t>(t), t, id <= 50 ? 1 : 0);
        const bool due = !parked.empty() && parked_at >= 0 && t - parked_at >= kWriteEvery;
        const bool writable = intermittent && ((t / kStep) % 3 == 0);
        if (!parked.empty() && (due || writable)) {
            commit(t);
        }
        // 内核仍不可写时，新的一轮会换掉停放帧。计时从第一次停放起算，不能因为换帧清零。
        if (!parked.empty()) {
            ledger.NoteReplaced(parked);
            parked.clear();
        }
        if (parked.empty()) {
            int64_t wait_ms = 0;
            parked = ledger.Pick(t, 16 * 1024, kEst, kPerFrame, &wait_ms);
            if (!parked.empty()) {
                ledger.NoteParked(parked);
                if (parked_at < 0)
                    parked_at = t;
            }
        }
        const int64_t sil = ledger.MaxSilenceMs(t);
        if (sil > max_silence)
            max_silence = sil;
    }
    if (!parked.empty())
        commit(40000);
    RunResult out;
    out.max_silence_ms = max_silence;
    for (int id = 1; id <= kEntities; ++id) {
        if (writes[static_cast<size_t>(id)] == 0)
            ++out.unwritten;
        else
            ++out.distinct_written;
    }
    return out;
}

}  // namespace

int main() {
    // 近处占四分之三名额，远处至少等 5 秒再进轮转。上界按远处名额估算。
    const int64_t bound = 5000 + ((150 + 10 - 1) / 10) * 1000 * 2;

    const RunResult legacy = Simulate(true, false);
    std::printf("legacy unwritten=%d silence_ms=%lld\n", legacy.unwritten,
                static_cast<long long>(legacy.max_silence_ms));
    Expect(legacy.unwritten >= 160, "legacy accounting writes only the first picked set");
    Expect(legacy.max_silence_ms >= 15000, "legacy silence grows for the whole run");

    const RunResult fixed = Simulate(false, false);
    std::printf("fixed unwritten=%d silence_ms=%lld bound=%lld\n", fixed.unwritten,
                static_cast<long long>(fixed.max_silence_ms), static_cast<long long>(bound));
    Expect(fixed.unwritten == 0, "every entity is written at least once");
    Expect(fixed.distinct_written == 200, "200 distinct entities written");
    Expect(fixed.max_silence_ms <= bound, "silence stays within two rotation periods");

    const RunResult mixed = Simulate(false, true);
    std::printf("intermittent unwritten=%d silence_ms=%lld\n", mixed.unwritten,
                static_cast<long long>(mixed.max_silence_ms));
    Expect(mixed.unwritten == 0, "intermittent socket still writes every entity");
    Expect(mixed.max_silence_ms <= bound, "intermittent socket does not starve an entity");

    auto union_after_replace = [](bool legacy) {
        AoiMoveCoalesceLedger ledger;
        ledger.EnableLegacyAccountingForTest(legacy);
        for (int id = 1; id <= 200; ++id)
            ledger.Offer(static_cast<uint64_t>(id), 1, 1, 0);
        std::vector<char> seen(201, 0);
        int covered = 0;
        for (int round = 0; round < 10; ++round) {
            int64_t wait_ms = 0;
            const auto picked = ledger.Pick(1000 + round * 200, 16 * 1024, 96, 40, &wait_ms);
            ledger.NoteReplaced(picked);
            for (const auto &stamp : picked) {
                if (stamp.entity_id <= 200 && !seen[static_cast<size_t>(stamp.entity_id)]) {
                    seen[static_cast<size_t>(stamp.entity_id)] = 1;
                    ++covered;
                }
            }
        }
        return covered;
    };
    const int legacy_cover = union_after_replace(true);
    const int fixed_cover = union_after_replace(false);
    std::printf("replace-only cover legacy=%d fixed=%d\n", legacy_cover, fixed_cover);
    Expect(legacy_cover == 40, "legacy replace keeps picking the same 40");
    Expect(fixed_cover == 200, "unwritten picks rotate through every entity");

    if (g_fail) {
        std::printf("FAIL aoi_move_rotate_test failures=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS aoi_move_rotate_test\n");
    return 0;
}
