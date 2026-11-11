#include "AoiMoveCoalesceLedger.h"
#include "AoiOutboundOrder.h"
#include "Logging.h"
#include "MapRuntime.h"
#include "MapStaticData.h"
#include "ServerStats.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// 任务 0。客户端按最坏顺序吃 AOI，不连 socket。
//
// 种子 1..20 的纯随机游走已经跑过：人从 (80,80) 出发，步长 ±8 米，40 步后仍在离开半径里，
// 没有人被 LEAVE。那一版对账是绿的，不能当证据。
//
// 失败种子 kSeed = 7。修复前的最小复现（UrgentDropsCoalescedMove 当时是 false）：
// 1. 观察者停在 (80,80)。实体 2 在 (82,80) 进入，先走一步。MOVE 只进合并槽和
//    AoiMoveCoalesceLedger，不 Flush。
// 2. 实体 2 再走到 (150,80)。呈现点滑出离开半径时，同一批先 Offer 这条 MOVE，再急送 LEAVE。
//    急送成功才走 ApplyUrgentToCoalesceSlot。UrgentDropsCoalescedMove 目前是 false，
//    槽里的 MOVE 还在。这条路径不改账本，PendingSince 也不清。
//    AoiStripEntityIds 只列出非 MOVE 的实体，给连接上还没写出第一个字节的那一帧用，
//    不碰合并槽，也不碰账本。本测试不走 SendBufV2。
// 3. LEAVE 写入成功后 NoteAoiWrite，服务端不再补发。过 300ms，ledger.Pick 仍选出实体 2。
//    Flush 把 MOVE 交给客户端。客户端对未知 MOVE 建模型。服务端可见集已经没有 2。这是 S1。
// 4. 实体 3：MOVE 进槽后走慢客户端清槽（拷进 resync，清槽和账本 pending）。
//    然后断线，这一批只有 LEAVE。确认之后按 ReplayClearedMoves(false) 把旧 MOVE Offer 回去再 Flush。
//    这是 S2。
// 5. 实体 4 走到 (20,20)。LEAVE 急送失败约 30 秒（测试时钟），然后恢复。
//    补发按 1s、2s、4s、5s 继续，恢复后的那次 LEAVE 把模型删掉。
// 6. 实体 5 按种子在进入半径里随机跨格。实体 6 断线再重连，ENTER 能补回来。
// 7. 另一张图上 8 个人挤在同一格，人数上限 2，用来看 S4。默认路径会把还在格子里的人塞回去。

namespace {

constexpr uint32_t kSeed = 7;
constexpr int kEnterCells = 1;
constexpr float kCell = 10.f;
constexpr int kCap = 20;
constexpr uint64_t kObs = 1;

struct Wire {
    int op = 0;
    uint64_t id = 0;
    uint64_t seq = 0;
    float x = 0;
    float z = 0;
};

struct HeldMove {
    uint64_t seq = 0;
    float x = 0;
    float z = 0;
    bool has = false;
};

struct Client {
    std::map<uint64_t, std::pair<float, float>> body;
    std::map<uint64_t, int> last_op;
    int leave_inside_fit = 0;
    int leave_inside_any = 0;
    int move_after_leave = 0;
    void Apply(const Wire &w, float ox, float oz, int inside_count, int cap, bool count_radius) {
        if (w.op == 3) {
            const int cheb = CellCheb(ox, oz, w.x, w.z);
            if (count_radius && cheb <= kEnterCells) {
                ++leave_inside_any;
                if (inside_count <= cap)
                    ++leave_inside_fit;
            }
            body.erase(w.id);
            last_op[w.id] = 3;
            return;
        }
        if (w.op == 2 && last_op[w.id] == 3)
            ++move_after_leave;
        body[w.id] = {w.x, w.z};
        last_op[w.id] = w.op;
    }
    static int CellCheb(float x0, float z0, float x1, float z1) {
        const int ax = static_cast<int>(std::floor(x0 / kCell));
        const int az = static_cast<int>(std::floor(z0 / kCell));
        const int bx = static_cast<int>(std::floor(x1 / kCell));
        const int bz = static_cast<int>(std::floor(z1 / kCell));
        return std::max(std::abs(ax - bx), std::abs(az - bz));
    }
};

struct Outbound {
    std::unordered_map<uint64_t, HeldMove> slot;
    AoiMoveCoalesceLedger ledger;
    std::vector<std::pair<uint64_t, HeldMove>> resync;
    AoiOutboundBooks books;
    std::vector<Wire> log;
    int64_t t = 10000;
    void OfferMove(uint64_t id, uint64_t seq, float x, float z) {
        if (TombstoneBlocksMove(books, id, t))
            return;
        books.notified.insert(id);
        auto &kept = slot[id];
        if (!KeepCoalescedMoveSeq(kept.has, kept.seq, seq))
            return;
        kept = HeldMove{seq, x, z, true};
        ledger.Offer(id, seq, t, 0, x, z, true);
    }
    void Urgent(const Wire &w, bool write_ok, Client *client, float ox, float oz, int inside, int cap,
                bool count_radius) {
        if (!write_ok || !client)
            return;
        ApplyUrgentAoi(&slot, &ledger,
                       [this](uint64_t id) {
                           resync.erase(std::remove_if(resync.begin(), resync.end(),
                                                       [id](const std::pair<uint64_t, HeldMove> &item) {
                                                           return item.first == id;
                                                       }),
                                        resync.end());
                       },
                       &books, w.op, w.id, t);
        client->Apply(w, ox, oz, inside, cap, count_radius);
        log.push_back(w);
    }
    int Flush(Client *client, float ox, float oz) {
        int sent = 0;
        for (int attempt = 0; attempt < 4 && sent == 0; ++attempt) {
            t += 300;
            int64_t wait_ms = 0;
            const auto picked = ledger.Pick(t, 150000, kAoiMoveEventBytes, 200, &wait_ms);
            std::vector<AoiMoveStamp> wrote;
            for (const auto &stamp : picked) {
                auto it = slot.find(stamp.entity_id);
                if (it == slot.end() || !it->second.has)
                    continue;
                if (it->second.seq != stamp.state_seq)
                    continue;
                Wire w;
                w.op = 2;
                w.id = stamp.entity_id;
                w.seq = it->second.seq;
                w.x = it->second.x;
                w.z = it->second.z;
                client->Apply(w, ox, oz, kCap, kCap, false);
                log.push_back(w);
                wrote.push_back(stamp);
                slot.erase(it);
                ++sent;
            }
            if (!wrote.empty())
                ledger.NoteKernelWrite(wrote, t, 64);
            if (sent > 0)
                break;
        }
        return sent;
    }
    void ClearToResync() {
        resync.clear();
        for (const auto &kv : slot)
            resync.push_back(kv);
        slot.clear();
        ledger.ClearPending();
    }
    void ReplayResync() {
        const auto kept = resync;
        resync.clear();
        for (const auto &item : kept) {
            if (!ShouldReplayClearedMove(false, books, item.first, t))
                continue;
            OfferMove(item.first, item.second.seq, item.second.x, item.second.z);
        }
    }
    bool Has(uint64_t id) const {
        auto it = slot.find(id);
        return it != slot.end() && it->second.has;
    }
};

std::shared_ptr<const MapStaticData> MakeMap() {
    const std::string json = R"JSON({
      "schema_version": 1, "map_template_id": 9101, "scene_name": "Open", "data_version": 1,
      "bounds_min": [0,0,0], "bounds_max": [160,5,160], "aoi_cell_size": 10.0, "nav_sample_step": 1.0,
      "grid_width": 160, "grid_height": 160, "walkable_rle": [1, 25600],
      "spawn_points": [{"id":"default","position":[80,0,80],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> d;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &d, &err)) {
        std::printf("FAIL map %s\n", err.c_str());
        return nullptr;
    }
    return d;
}

MapEntity Ent(uint64_t id, float x, float z) {
    MapEntity e;
    e.player_id = id;
    e.player_name = "b";
    e.x = x;
    e.y = 0;
    e.z = z;
    e.hp = 100;
    e.max_hp = 100;
    e.move_speed = 10.f;
    e.gateway_instance_id = "gw";
    e.session_id = "s";
    return e;
}

int InsideEnter(MapRuntime &rt, uint64_t map, float ox, float oz, int radius_cells) {
    std::vector<std::pair<float, float>> xz;
    rt.CollectXz(map, 0, &xz);
    int n = 0;
    for (const auto &p : xz) {
        if (Client::CellCheb(ox, oz, p.first, p.second) <= radius_cells)
            ++n;
    }
    return n;
}

struct FeedOpt {
    std::set<uint64_t> drop_leave;
    bool count_radius = true;
    int cap = kCap;
    uint64_t watch = 0;
    int watched_leaves = 0;
};

void Feed(MapRuntime &rt, const AoiPushBatch &batch, uint64_t obs, Outbound *out, Client *client,
          float ox, float oz, FeedOpt *opt) {
    const int inside = InsideEnter(rt, batch.map_instance_id == 0 ? 1 : batch.map_instance_id, ox, oz,
                                    kEnterCells);
    for (const auto &ev : batch.events) {
        if (ev.recipient_id != obs)
            continue;
        Wire w;
        w.op = ev.op;
        w.id = ev.snapshot.player_id;
        w.seq = ev.snapshot.state_seq;
        w.x = ev.snapshot.aoi_live ? ev.snapshot.aoi_x : ev.snapshot.x;
        w.z = ev.snapshot.aoi_live ? ev.snapshot.aoi_z : ev.snapshot.z;
        if (w.op == 2) {
            out->OfferMove(w.id, w.seq, w.x, w.z);
            continue;
        }
        const bool drop = w.op == 3 && opt && opt->drop_leave.count(w.id) != 0;
        if (w.op == 3 && opt && w.id == opt->watch)
            ++opt->watched_leaves;
        const bool ok = !drop;
        out->Urgent(w, ok, client, ox, oz, inside, opt ? opt->cap : kCap, opt ? opt->count_radius : true);
        if (ok && w.op == 3 && ev.notify_version != 0)
            rt.NoteAoiWrite(obs, w.id, 3, true, ev.notify_version);
    }
}

void FeedAll(MapRuntime &rt, const std::vector<AoiPushBatch> &batches, uint64_t obs, Outbound *out,
             Client *client, float ox, float oz, FeedOpt *opt) {
    for (const auto &b : batches)
        Feed(rt, b, obs, out, client, ox, oz, opt);
}

bool MoveTo(MapRuntime &rt, uint64_t map, uint64_t id, float x, float z, uint64_t *seq, int64_t *now,
            AoiPushBatch *pushes, std::string *err) {
    *now += 250;
    *pushes = {};
    MapEntity conf;
    return rt.Move(map, id, x, 0.f, z, 0.f, (*seq)++, *now, &conf, pushes, err, true) ==
           MapMoveReject::Ok;
}

bool GlideUntilLeave(MapRuntime &rt, uint64_t id, Outbound *out, Client *client, FeedOpt *opt) {
    for (int i = 0; i < 90; ++i) {
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        const int before = opt->watched_leaves;
        FeedAll(rt, more, kObs, out, client, 80.f, 80.f, opt);
        if (opt->watched_leaves > before)
            return true;
        (void)id;
    }
    return false;
}

std::set<uint64_t> ServerView(MapRuntime &rt, uint64_t player) {
    MapEntity self;
    std::vector<MapEntity> seen;
    uint64_t mid = 0;
    rt.SnapshotView(player, &self, &seen, &mid);
    std::set<uint64_t> ids;
    for (const auto &e : seen)
        ids.insert(e.player_id);
    return ids;
}

std::string Join(const std::vector<uint64_t> &ids) {
    std::string s;
    for (uint64_t id : ids) {
        if (!s.empty())
            s += ",";
        s += std::to_string(id);
    }
    return s.empty() ? "-" : s;
}

void Diff(const std::set<uint64_t> &got, const std::set<uint64_t> &server, std::vector<uint64_t> *ghosts,
          std::vector<uint64_t> *missing) {
    std::set_difference(got.begin(), got.end(), server.begin(), server.end(), std::back_inserter(*ghosts));
    std::set_difference(server.begin(), server.end(), got.begin(), got.end(), std::back_inserter(*missing));
}

int CrowdLeaveInside(MapRuntime &rt, const std::shared_ptr<const MapStaticData> &data) {
    Client client;
    Outbound out;
    FeedOpt opt;
    opt.cap = 2;
    const uint64_t obs = 101;
    std::string err;
    uint64_t seq = 1;
    int64_t now = 8000;
    for (int i = 0; i < 9; ++i) {
        const uint64_t id = 101 + static_cast<uint64_t>(i);
        const float x = 40.f + static_cast<float>(i) * 0.2f;
        AoiPushBatch pushes;
        MapEntity self;
        std::vector<MapEntity> snap;
        if (!rt.Enter(2, data, Ent(id, x, 40.f), &self, &snap, &pushes, &err, kEnterCells, 2))
            return -1;
        if (id != obs)
            Feed(rt, pushes, obs, &out, &client, 40.f, 40.f, &opt);
    }
    for (int n = 0; n < 5; ++n) {
        AoiPushBatch pushes;
        if (!MoveTo(rt, 2, 102, 42.f, 41.f, &seq, &now, &pushes, &err))
            break;
        Feed(rt, pushes, obs, &out, &client, 40.f, 40.f, &opt);
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        FeedAll(rt, more, obs, &out, &client, 40.f, 40.f, &opt);
    }
    return client.leave_inside_any;
}

bool RunSeed(uint32_t seed) {
    ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    ::unsetenv("GAMEMESH_AOI_V2_HYSTERESIS");
    ::unsetenv("GAMEMESH_AOI_V2_LAYERS");
    auto data = MakeMap();
    if (!data)
        return false;
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(kEnterCells);
    rt.SetMaxVisible(kCap);

    Outbound out;
    Client client;
    FeedOpt opt;
    std::string err;
    uint64_t seq = 1;
    int64_t now = 5000;
    const int n = 6;
    for (int i = 0; i < n; ++i) {
        const uint64_t id = static_cast<uint64_t>(i + 1);
        const float x = id == kObs ? 80.f : 82.f;
        AoiPushBatch pushes;
        MapEntity self;
        std::vector<MapEntity> snap;
        if (!rt.Enter(1, data, Ent(id, x, 80.f), &self, &snap, &pushes, &err, kEnterCells, kCap)) {
            std::printf("FAIL enter id=%llu %s\n", static_cast<unsigned long long>(id), err.c_str());
            return false;
        }
        Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    }

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> step(-6.f, 6.f);
    for (int k = 0; k < 8; ++k) {
        float x = 80.f + step(rng);
        float z = 80.f + step(rng);
        if (x < 74.f)
            x = 74.f;
        if (x > 96.f)
            x = 96.f;
        if (z < 74.f)
            z = 74.f;
        if (z > 96.f)
            z = 96.f;
        AoiPushBatch pushes;
        if (!MoveTo(rt, 1, 5, x, z, &seq, &now, &pushes, &err))
            continue;
        Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        FeedAll(rt, more, kObs, &out, &client, 80.f, 80.f, &opt);
    }

    AoiPushBatch pushes;
    if (!MoveTo(rt, 1, 2, 86.f, 82.f, &seq, &now, &pushes, &err)) {
        std::printf("FAIL seed=%u step=park move\n", seed);
        return false;
    }
    Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    if (!MoveTo(rt, 1, 2, 150.f, 80.f, &seq, &now, &pushes, &err)) {
        std::printf("FAIL seed=%u step=far move\n", seed);
        return false;
    }
    Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    opt.watch = 2;
    if (!GlideUntilLeave(rt, 2, &out, &client, &opt)) {
        std::printf("FAIL seed=%u no LEAVE for entity 2\n", seed);
        return false;
    }
    const bool slot_kept = out.Has(2);
    const bool ledger_kept = out.ledger.PendingSinceForTest(2) > 0;
    const int flushed = out.Flush(&client, 80.f, 80.f);
    const bool ghost2 = client.body.count(2) != 0 && ServerView(rt, kObs).count(2) == 0;

    if (!MoveTo(rt, 1, 3, 86.f, 84.f, &seq, &now, &pushes, &err)) {
        std::printf("FAIL seed=%u step=entity 3 move\n", seed);
        return false;
    }
    Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    {
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        FeedAll(rt, more, kObs, &out, &client, 80.f, 80.f, &opt);
    }
    const bool parked3 = out.Has(3);
    out.ClearToResync();
    AoiPushBatch bye;
    if (!rt.Disconnect(3, &bye)) {
        std::printf("FAIL seed=%u disconnect 3\n", seed);
        return false;
    }
    opt.count_radius = false;
    Feed(rt, bye, kObs, &out, &client, 80.f, 80.f, &opt);
    opt.count_radius = true;
    const bool slot_empty_after_leave = !out.Has(3);
    out.ReplayResync();
    const bool replayed3 = out.Has(3) && out.ledger.PendingSinceForTest(3) > 0;
    out.Flush(&client, 80.f, 80.f);
    const bool ghost3 = client.body.count(3) != 0 && ServerView(rt, kObs).count(3) == 0;

    opt.drop_leave.insert(4);
    opt.watch = 4;
    opt.watched_leaves = 0;
    MapRuntime::SetNowMsForTest(100000);
    if (!MoveTo(rt, 1, 4, 20.f, 20.f, &seq, &now, &pushes, &err)) {
        std::printf("FAIL seed=%u step=entity 4 move\n", seed);
        return false;
    }
    Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    if (!GlideUntilLeave(rt, 4, &out, &client, &opt)) {
        std::printf("FAIL seed=%u no LEAVE for entity 4\n", seed);
        return false;
    }
    int quiet = 0;
    const int64_t gaps[] = {1000, 2000, 4000, 5000, 5000, 5000, 8000};
    int64_t clock = 100000;
    for (int64_t gap : gaps) {
        clock += gap;
        MapRuntime::SetNowMsForTest(clock);
        const int before = opt.watched_leaves;
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        FeedAll(rt, more, kObs, &out, &client, 80.f, 80.f, &opt);
        if (opt.watched_leaves == before)
            ++quiet;
    }
    opt.drop_leave.erase(4);
    clock += 5000;
    MapRuntime::SetNowMsForTest(clock);
    {
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        FeedAll(rt, more, kObs, &out, &client, 80.f, 80.f, &opt);
    }
    out.Flush(&client, 80.f, 80.f);
    const bool ghost4 = client.body.count(4) != 0 && ServerView(rt, kObs).count(4) == 0;

    if (!MoveTo(rt, 1, 6, 84.f, 83.f, &seq, &now, &pushes, &err)) {
        std::printf("FAIL seed=%u step=entity 6 move\n", seed);
        return false;
    }
    Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    bye = {};
    opt.count_radius = false;
    if (!rt.Disconnect(6, &bye)) {
        std::printf("FAIL seed=%u disconnect 6\n", seed);
        return false;
    }
    Feed(rt, bye, kObs, &out, &client, 80.f, 80.f, &opt);
    out.Flush(&client, 80.f, 80.f);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch back;
    if (!rt.Reconnect(6, "gw", "s6", &self, &snap, &back)) {
        std::printf("FAIL seed=%u reconnect 6\n", seed);
        return false;
    }
    opt.count_radius = true;
    Feed(rt, back, kObs, &out, &client, 80.f, 80.f, &opt);
    out.Flush(&client, 80.f, 80.f);

    const int crowd = CrowdLeaveInside(rt, data);
    const auto server = ServerView(rt, kObs);
    std::set<uint64_t> got;
    for (const auto &kv : client.body)
        got.insert(kv.first);
    std::vector<uint64_t> ghosts;
    std::vector<uint64_t> missing;
    Diff(got, server, &ghosts, &missing);

    std::vector<uint64_t> order_ghosts;
    for (uint64_t id : ghosts) {
        if (id != 4)
            order_ghosts.push_back(id);
    }
    const bool bad = !order_ghosts.empty() || !missing.empty() || client.leave_inside_fit != 0 || slot_kept ||
                     ledger_kept || ghost2 || ghost3 || ghost4;
    std::printf(
        "seed=%u order_ghosts=%s s3_ghost4=%d missing=%s leave_inside_fit=%d leave_inside_any=%d move_after_leave=%d\n",
        seed, Join(order_ghosts).c_str(), ghost4 ? 1 : 0, Join(missing).c_str(), client.leave_inside_fit,
        client.leave_inside_any, client.move_after_leave);
    std::printf(
        "  S1 slot_kept=%d ledger_pending=%d flush_sent=%d ghost2=%d\n"
        "  S2 parked=%d slot_empty_on_leave=%d replayed=%d ghost3=%d\n"
        "  S3 leave_emits=%d quiet_ticks=%d ghost4=%d\n"
        "  S4 crowd_leave_inside=%d\n"
        "  UrgentDropsCoalescedMove(3)=%d ReplayClearedMoves(false)=%d\n",
        slot_kept ? 1 : 0, ledger_kept ? 1 : 0, flushed, ghost2 ? 1 : 0, parked3 ? 1 : 0,
        slot_empty_after_leave ? 1 : 0, replayed3 ? 1 : 0, ghost3 ? 1 : 0, opt.watched_leaves, quiet,
        ghost4 ? 1 : 0, crowd, UrgentDropsCoalescedMove(3) ? 1 : 0, ReplayClearedMoves(false) ? 1 : 0);
    rt.ClearForTest();
    return !bad;
}

bool RunExpiry() {
    ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    auto data = MakeMap();
    if (!data)
        return false;
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(kEnterCells);
    rt.SetMaxVisible(kCap);
    MapRuntime::SetNowMsForTest(300000);
    Outbound out;
    Client client;
    FeedOpt opt;
    opt.drop_leave.insert(2);
    opt.watch = 2;
    std::string err;
    uint64_t seq = 1;
    int64_t now = 1;
    for (int i = 0; i < 2; ++i) {
        AoiPushBatch pushes;
        MapEntity self;
        std::vector<MapEntity> snap;
        const uint64_t id = static_cast<uint64_t>(i + 1);
        if (!rt.Enter(1, data, Ent(id, id == 1 ? 80.f : 82.f, 80.f), &self, &snap, &pushes, &err,
                      kEnterCells, kCap))
            return false;
        Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    }
    AoiPushBatch pushes;
    if (!MoveTo(rt, 1, 2, 150.f, 80.f, &seq, &now, &pushes, &err))
        return false;
    Feed(rt, pushes, kObs, &out, &client, 80.f, 80.f, &opt);
    if (!GlideUntilLeave(rt, 2, &out, &client, &opt)) {
        std::printf("FAIL expiry never left\n");
        return false;
    }
    const int leaves = opt.watched_leaves;
    const uint64_t before = ServerStats::aoi_leave_retry_expired.load(std::memory_order_relaxed);
    MapRuntime::SetNowMsForTest(300000 + 600000);
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    FeedAll(rt, more, kObs, &out, &client, 80.f, 80.f, &opt);
    const uint64_t after = ServerStats::aoi_leave_retry_expired.load(std::memory_order_relaxed);
    more.clear();
    rt.AdvancePresented(&more);
    const int mid = opt.watched_leaves;
    FeedAll(rt, more, kObs, &out, &client, 80.f, 80.f, &opt);
    const bool stopped = opt.watched_leaves == mid;
    const bool still = client.body.count(2) != 0;
    std::printf("expiry before=%llu after=%llu leaves=%d stopped=%d still=%d\n",
                static_cast<unsigned long long>(before), static_cast<unsigned long long>(after), leaves,
                stopped ? 1 : 0, still ? 1 : 0);
    rt.ClearForTest();
    return after > before && stopped && still;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::ERROR);
    ::unsetenv("GAMEMESH_AOI_MEMBERSHIP_UNIFIED");
    const uint64_t base = ServerStats::aoi_unexpected_leave.load(std::memory_order_relaxed);
    if (!RunSeed(kSeed) || !RunExpiry()) {
        std::printf("FAIL aoi_client_model_test seed=%u\n", kSeed);
        return 1;
    }
    const uint64_t off = ServerStats::aoi_unexpected_leave.load(std::memory_order_relaxed) - base;
    ::setenv("GAMEMESH_AOI_MEMBERSHIP_UNIFIED", "1", 1);
    const uint64_t mid = ServerStats::aoi_unexpected_leave.load(std::memory_order_relaxed);
    const bool unified_ok = RunSeed(kSeed);
    const uint64_t on = ServerStats::aoi_unexpected_leave.load(std::memory_order_relaxed) - mid;
    ::unsetenv("GAMEMESH_AOI_MEMBERSHIP_UNIFIED");
    std::printf("unexpected_leave unified_off=%llu unified_on=%llu\n",
                static_cast<unsigned long long>(off), static_cast<unsigned long long>(on));
    if (!unified_ok) {
        std::printf("FAIL aoi_client_model_test unified seed=%u\n", kSeed);
        return 1;
    }
    std::printf("OK aoi_client_model_test seed=%u\n", kSeed);
    return 0;
}
