#include "AoiHoldGate.h"
#include "AoiOutboundOrder.h"
#include "AoiStrip.h"
#include "ProtoFraming.h"
#include "game.pb.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

int g_fail = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++g_fail;
    }
}

std::string FrameOf(uint64_t id_a, int op_a, uint64_t id_b, int op_b) {
    game::GameResponse gr;
    gr.set_ok(true);
    auto *d = gr.mutable_aoi_delta();
    d->set_map_instance_id(1);
    auto *ea = d->add_events();
    ea->set_op(op_a);
    ea->mutable_entity()->set_player_id(id_a);
    ea->mutable_entity()->set_state_seq(3);
    if (id_b != 0) {
        auto *eb = d->add_events();
        eb->set_op(op_b);
        eb->mutable_entity()->set_player_id(id_b);
        eb->mutable_entity()->set_state_seq(4);
    }
    std::string body;
    std::string frame;
    gr.SerializeToString(&body);
    gameproto::EncodeFrame(body, &frame);
    return frame;
}

void TestSlotAndTombstone() {
    std::unordered_map<uint64_t, int> slot;
    slot[2] = 9;
    AoiMoveCoalesceLedger ledger;
    ledger.Offer(2, 9, 1000, 0);
    std::vector<uint64_t> resync{2, 5};
    AoiOutboundBooks books;
    books.notified.insert(2);
    books.notified.insert(5);
    ApplyUrgentAoi(&slot, &ledger,
                   [&](uint64_t id) {
                       resync.erase(std::remove(resync.begin(), resync.end(), id), resync.end());
                   },
                   &books, 3, 2, 2000);
    Expect(slot.count(2) == 0, "LEAVE erases the coalesced move");
    Expect(ledger.PendingSinceForTest(2) == 0, "LEAVE forgets the ledger entry");
    Expect(std::find(resync.begin(), resync.end(), 2) == resync.end(), "LEAVE erases resync");
    Expect(std::find(resync.begin(), resync.end(), 5) != resync.end(), "other resync entity stays");
    Expect(TombstoneBlocksMove(books, 2, 2000 + 1000), "tombstone blocks MOVE inside the window");
    Expect(!TombstoneBlocksMove(books, 2, 2000 + AoiMoveTombstoneMs()), "tombstone expires");
    NoteEnterVisible(&books, 2);
    Expect(!TombstoneBlocksMove(books, 2, 2000 + 1000), "ENTER lifts the tombstone");
    Expect(!ShouldReplayClearedMove(false, books, 7, 3000), "resync skips an entity that already left");
    books.notified.insert(5);
    Expect(ShouldReplayClearedMove(false, books, 5, 3000), "resync keeps an entity still notified");
    Expect(!ShouldReplayClearedMove(true, books, 5, 3000), "visible-set switch still drops the whole resync");
}

void TestMixedFrame() {
    const std::string frame = FrameOf(2, 2, 8, 2);
    std::string out;
    bool empty = false;
    int removed = 0;
    Expect(RewriteAoiFrameDroppingEntities(frame, {2}, &out, &empty, &removed), "mixed frame rewrites");
    Expect(removed == 1 && !empty && !out.empty(), "the other entity stays in the frame");
    std::string payload;
    size_t consumed = 0;
    Expect(gameproto::DecodeOneFrameFrom(out, 0, &payload, &consumed) == gameproto::FrameDecodeResult::Complete,
           "rewritten frame decodes");
    game::GameResponse gr;
    Expect(gr.ParseFromString(payload) && gr.aoi_delta().events_size() == 1, "one event remains");
    Expect(gr.aoi_delta().events(0).entity().player_id() == 8, "remaining event is the other entity");

    std::string gone;
    bool all = false;
    int n = 0;
    Expect(RewriteAoiFrameDroppingEntities(frame, {2, 8}, &gone, &all, &n) && all && n == 2,
           "a frame with only the stripped entities becomes empty");
}

void TestHoldOrder() {
    for (int trial = 0; trial < 200; ++trial) {
        AoiHoldGate<int> gate;
        gate.Hold(7);
        Expect(gate.Admit(7, 1), "held viewer parks the old event");
        std::mutex mu;
        std::vector<int> q;
        std::atomic<bool> go{false};
        std::thread releaser([&] {
            while (!go.load()) {
            }
            gate.Release(7, [&](std::vector<int> batch) {
                std::lock_guard<std::mutex> lk(mu);
                q.insert(q.end(), batch.begin(), batch.end());
            });
        });
        std::thread publisher([&] {
            while (!go.load()) {
            }
            for (int i = 0; i < 40; ++i) {
                if (!gate.Admit(7, 2)) {
                    std::lock_guard<std::mutex> lk(mu);
                    q.push_back(2);
                }
            }
        });
        go.store(true);
        releaser.join();
        publisher.join();
        const auto old = std::find(q.begin(), q.end(), 1);
        Expect(old != q.end(), "the held event is flushed");
        Expect(std::find(q.begin(), old, 2) == old, "a newer event is not queued before the held one");
    }
}

}  // namespace

int main() {
    TestSlotAndTombstone();
    TestMixedFrame();
    TestHoldOrder();
    if (g_fail) {
        std::printf("FAIL aoi_leave_slot_test failures=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS aoi_leave_slot_test\n");
    return 0;
}
