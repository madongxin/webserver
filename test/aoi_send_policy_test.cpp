#include "AoiMoveCoalesceLedger.h"
#include "SlowClientPolicy.h"

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

void SeedWritten(AoiMoveCoalesceLedger *ledger, int n, int64_t now) {
    for (int id = 1; id <= n; ++id)
        ledger->Offer(static_cast<uint64_t>(id), 1, now, id <= 40 ? 1 : 0);
    std::vector<AoiMoveStamp> left;
    for (int id = 1; id <= n; ++id)
        left.push_back(AoiMoveStamp{static_cast<uint64_t>(id), 1});
    ledger->NoteParked(left);
    ledger->NoteKernelWrite(left, now, n * 54);
    for (int id = 1; id <= n; ++id)
        ledger->Offer(static_cast<uint64_t>(id), 2, now + 1, id <= 40 ? 1 : 0);
}

void TestSteadyRotation() {
    AoiMoveCoalesceLedger ledger;
    SeedWritten(&ledger, 200, 1000);
    std::vector<int> seen(201, 0);
    int covered = 0;
    std::vector<uint64_t> first;
    for (int round = 0; round < 20; ++round) {
        int64_t wait_ms = 0;
        const auto picked = ledger.Pick(5000 + round * 200, 16 * 1024, 54, 40, &wait_ms);
        if (round == 0) {
            for (const auto &s : picked)
                first.push_back(s.entity_id);
        }
        int same = 0;
        for (const auto &s : picked) {
            if (!first.empty()) {
                for (uint64_t id : first)
                    if (id == s.entity_id)
                        ++same;
            }
            if (s.entity_id <= 200 && !seen[s.entity_id]) {
                seen[s.entity_id] = 1;
                ++covered;
            }
        }
        if (round == 1)
            Expect(same < static_cast<int>(picked.size()), "replaced batch is not picked again immediately");
        ledger.NoteReplaced(picked);
    }
    std::printf("steady cover=%d\n", covered);
    Expect(covered == 200, "steady rotation reaches every entity");
}

void TestForceStaysInsideBudget() {
    AoiMoveCoalesceLedger ledger;
    constexpr int kBudget = 331;
    constexpr int kEst = 54;
    int64_t now = 1000;
    for (int step = 0; step < 300; ++step) {
        for (int id = 1; id <= 200; ++id)
            ledger.Offer(static_cast<uint64_t>(id), static_cast<uint64_t>(step + 1), now, 0);
        int64_t wait_ms = 0;
        auto picked = ledger.Pick(now, kBudget, kEst, 40, &wait_ms);
        if (!picked.empty()) {
            ledger.NoteParked(picked);
            const int payload = static_cast<int>(picked.size()) * kEst;
            ledger.NoteKernelWrite(picked, now, payload);
        }
        now += 200;
    }
    const int cap = kBudget * 60 + kBudget * 60 / 10;
    std::printf("force charged=%d cap=%d\n", ledger.charged_total(), cap);
    Expect(ledger.charged_total() <= cap, "force does not exceed budget by more than 10 percent");
}

void TestTightBudgetSendsNow() {
    AoiMoveCoalesceLedger ledger;
    for (int id = 1; id <= 200; ++id)
        ledger.Offer(static_cast<uint64_t>(id), 1, 1000, 0);
    int64_t wait_ms = 0;
    const auto picked = ledger.Pick(1000, 331, 38, 40, &wait_ms);
    std::printf("tight budget picked=%zu wait=%lld\n", picked.size(), static_cast<long long>(wait_ms));
    Expect(picked.size() == 6, "331 B/s sends the six entities that fit, without waiting for eight");
}

void TestHeaderAccounted() {
    AoiMoveCoalesceLedger ledger;
    ledger.Offer(1, 1, 1000, 1);
    int64_t wait_ms = 0;
    auto picked = ledger.Pick(1200, 10000, 54, 40, &wait_ms);
    Expect(!picked.empty(), "one event is picked");
    ledger.NoteParked(picked);
    ledger.NoteKernelWrite(picked, 1000, 54);
    Expect(ledger.charged_total() == 54 + kOnWireHeaderBytes, "wire charge adds the 66 byte header");
}

void TestSlowClientLevels() {
    SlowClientTracker slow;
    Expect(slow.Evaluate(0, false) == SlowClientAction::None, "fresh connection is not slow");
    Expect(slow.Evaluate(10000, false) == SlowClientAction::Degrade, "10s without a move write degrades");
    Expect(slow.degraded(), "degrade sticks until a move is written");
    Expect(slow.Evaluate(30000, false) == SlowClientAction::ClearSlot, "30s clears the slot");
    Expect(slow.needs_resync(), "clear marks a snapshot resync");
    slow.NoteMoveWritten(30000);
    Expect(slow.snapshot_due(), "first move write after clear asks for a snapshot");
    Expect(!slow.degraded(), "a move write clears degrade");
    SlowClientTracker dead;
    Expect(dead.Evaluate(0, false) == SlowClientAction::None, "close tracker starts clean");
    Expect(dead.Evaluate(90000, false) == SlowClientAction::Close, "90s without any byte closes");
    SlowClientTracker overflow;
    Expect(overflow.Evaluate(1000, true) == SlowClientAction::Close, "control queue overflow closes");
    SlowClientTracker replies;
    replies.Evaluate(0, false);
    replies.NoteAnyByteWritten(50000);
    Expect(replies.Evaluate(90000, false) != SlowClientAction::Close,
           "bytes written for replies reset the close timer");
    SlowClientTracker budget;
    budget.Evaluate(0, false);
    budget.NoteServerDeferred(10000);
    Expect(budget.Evaluate(10000, false) == SlowClientAction::None,
           "a budget defer is not a slow client");
    Expect(budget.Evaluate(90000, false) == SlowClientAction::Close,
           "no bytes at all still closes after the budget defer");
}

}  // namespace

int main() {
    TestSteadyRotation();
    TestForceStaysInsideBudget();
    TestTightBudgetSendsNow();
    TestHeaderAccounted();
    TestSlowClientLevels();
    if (g_fail) {
        std::printf("FAIL aoi_send_policy_test failures=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS aoi_send_policy_test\n");
    return 0;
}
