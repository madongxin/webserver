/**
 * P2 DUNGEON：CreateDungeon + 队员校验 + 空本回收。
 */
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int Fail(const char *msg) {
    std::printf("FAIL %s\n", msg);
    return 1;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool InitRedisPoolFromCnf() {
    const std::string &path = RedisConfigPath::RedisCnf();
    std::ifstream in(path);
    if (!in)
        return false;
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "ip")
            host = val;
        else if (key == "port")
            port = std::atoi(val.c_str());
        else if (key == "password")
            password = val;
    }
    return RedisPool::Instance().Init(host, port, password, 8);
}

}  // namespace

int main() {
    if (!InitRedisPoolFromCnf()) {
        std::printf("FAIL map_dungeon_test (Redis unavailable)\n");
        return 1;
    }
    const std::string prefix =
        "gamemesh:test:map_dungeon:" + std::to_string(static_cast<unsigned long long>(::getpid())) +
        ":";
    if (!PlacementStore::Instance().InitFromSessionPrefix(prefix))
        return Fail("PlacementStore init");
    PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});

    const uint64_t tpl = 2101000ULL + static_cast<uint64_t>(::getpid() % 100000);
    const uint64_t leader = 81001;
    const uint64_t member = 81002;
    const uint64_t stranger = 81003;

    CreateDungeonInput cin;
    cin.realm_id = 1;
    cin.map_template_id = tpl;
    cin.player_id = leader;
    cin.member_player_ids = {member};
    cin.operation_id = "dungeon-op-1";
    cin.hard_cap = 5;
    cin.empty_close_delay = 1;
    CreateDungeonResult created;
    if (!PlacementStore::Instance().CreateDungeon(cin, &created) || !created.ok)
        return Fail("create dungeon");
    if (created.placement.kind != "DUNGEON" || created.placement.map_instance_id == 0)
        return Fail("create kind/id");
    bool have_leader = false, have_member = false;
    for (uint64_t pid : created.member_player_ids) {
        if (pid == leader)
            have_leader = true;
        if (pid == member)
            have_member = true;
    }
    if (!have_leader || !have_member)
        return Fail("members must include leader and invitee");

    CreateDungeonResult again;
    if (!PlacementStore::Instance().CreateDungeon(cin, &again) || !again.ok)
        return Fail("create dungeon idempotent");
    if (!again.idempotent_hit ||
        again.placement.map_instance_id != created.placement.map_instance_id)
        return Fail("idempotent instance");

    ResolveOrCreateInput enter;
    enter.realm_id = 1;
    enter.map_template_id = tpl;
    enter.map_instance_id = created.placement.map_instance_id;
    enter.player_id = stranger;
    enter.kind = "DUNGEON";
    enter.operation_id = "enter-stranger";
    ResolveOrCreateResult bad;
    if (PlacementStore::Instance().ResolveOrCreate(enter, &bad) || bad.ok)
        return Fail("stranger should fail");
    if (bad.error_code != "ERR_DUNGEON_NOT_MEMBER")
        return Fail("stranger code");

    enter.player_id = member;
    enter.operation_id = "enter-member";
    ResolveOrCreateResult okm;
    if (!PlacementStore::Instance().ResolveOrCreate(enter, &okm) || !okm.ok)
        return Fail("member enter");
    if (okm.placement.map_instance_id != created.placement.map_instance_id)
        return Fail("member instance");

    if (!PlacementStore::Instance().ReleaseByPlayer(member))
        return Fail("release member");

    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::vector<uint64_t> closed;
    if (!PlacementStore::Instance().CloseIdleInstances(now, 16, &closed))
        return Fail("close idle too early call");
    if (!closed.empty())
        return Fail("must not close before delay");

    if (!PlacementStore::Instance().CloseIdleInstances(now + 2, 16, &closed))
        return Fail("close idle");
    if (closed.size() != 1 || closed[0] != created.placement.map_instance_id)
        return Fail("closed id");

    PlacementRecord rec;
    if (!PlacementStore::Instance().Get(created.placement.map_instance_id, &rec) ||
        rec.state != PlacementState::Closed)
        return Fail("state CLOSED");

    enter.player_id = member;
    enter.operation_id = "enter-after-close";
    ResolveOrCreateResult gone;
    if (PlacementStore::Instance().ResolveOrCreate(enter, &gone) || gone.ok)
        return Fail("enter after close should fail");
    if (gone.error_code != "ERR_DUNGEON_NOT_FOUND")
        return Fail("after close code");

    // LINE 空线：两条线时才关空线
    {
        const uint64_t ltpl = tpl + 9;
        auto line_in = [&](uint64_t player, uint32_t line_no = 0) {
            ResolveOrCreateInput in;
            in.realm_id = 1;
            in.map_template_id = ltpl;
            in.player_id = player;
            in.kind = "LINE";
            in.line_no = line_no;
            in.soft_cap = 1;
            in.hard_cap = 1;
            in.max_lines = 8;
            in.min_lines = 1;
            in.empty_close_delay = 1;
            in.operation_id = "line-idle-" + std::to_string(player);
            return in;
        };
        ResolveOrCreateResult a, b;
        if (!PlacementStore::Instance().ResolveOrCreate(line_in(82001), &a) || !a.ok)
            return Fail("line idle a");
        if (!PlacementStore::Instance().ResolveOrCreate(line_in(82002), &b) || !b.ok)
            return Fail("line idle b");
        if (a.placement.line_no == b.placement.line_no)
            return Fail("need two lines");
        PlacementStore::Instance().ReleaseByPlayer(82002);
        const int64_t lnow = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        std::vector<uint64_t> lc;
        if (!PlacementStore::Instance().CloseIdleInstances(lnow + 2, 16, &lc))
            return Fail("line close idle");
        bool found = false;
        for (uint64_t id : lc) {
            if (id == b.placement.map_instance_id)
                found = true;
            if (id == a.placement.map_instance_id)
                return Fail("must keep min_lines line");
        }
        if (!found)
            return Fail("empty extra line not closed");
        PlacementStore::Instance().ReleaseByPlayer(82001);
        lc.clear();
        if (!PlacementStore::Instance().CloseIdleInstances(lnow + 4, 16, &lc))
            return Fail("line last close call");
        for (uint64_t id : lc) {
            if (id == a.placement.map_instance_id)
                return Fail("last line must stay for min_lines");
        }
    }

    // 整图无人时立刻关掉多余空线（不等 empty_close_delay）；幽灵占位可 reclaim。
    {
        const uint64_t etpl = tpl + 11;
        auto ein = [&](uint64_t player, uint32_t line_no = 0) {
            ResolveOrCreateInput in;
            in.realm_id = 1;
            in.map_template_id = etpl;
            in.player_id = player;
            in.kind = "LINE";
            in.line_no = line_no;
            in.soft_cap = 1;
            in.hard_cap = 1;
            in.max_lines = 8;
            in.min_lines = 1;
            in.empty_close_delay = 300;
            in.operation_id = "line-empty-all-" + std::to_string(player);
            return in;
        };
        ResolveOrCreateResult a, b;
        if (!PlacementStore::Instance().ResolveOrCreate(ein(82101), &a) || !a.ok)
            return Fail("empty-all a");
        if (!PlacementStore::Instance().ResolveOrCreate(ein(82102), &b) || !b.ok)
            return Fail("empty-all b");
        if (a.placement.line_no == b.placement.line_no)
            return Fail("empty-all need two lines");
        PlacementStore::Instance().ReleaseByPlayer(82101);
        PlacementStore::Instance().ReleaseByPlayer(82102);
        const int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        std::vector<uint64_t> closed;
        if (!PlacementStore::Instance().CloseIdleInstances(now, 16, &closed))
            return Fail("empty-all close");
        bool extra_closed = false;
        for (uint64_t id : closed) {
            if (id == a.placement.map_instance_id || id == b.placement.map_instance_id)
                extra_closed = true;
        }
        if (!extra_closed)
            return Fail("empty template must close extra line without delay");
        if (closed.size() > 1)
            return Fail("must keep min_lines when template empty");
        std::vector<MapLineInfo> leftover;
        if (!PlacementStore::Instance().ListLines(1, etpl, &leftover))
            return Fail("empty-all list");
        uint32_t ready_n = 0, leftover_ln = 0;
        for (const auto &r : leftover) {
            if (r.state == "READY") {
                ++ready_n;
                leftover_ln = r.line_no;
            }
        }
        if (ready_n != 1 || leftover_ln != 1)
            return Fail("empty leftover must be line 1");
    }
    {
        ResolveOrCreateInput in;
        in.realm_id = 1;
        in.map_template_id = tpl + 12;
        in.player_id = 82201;
        in.kind = "LINE";
        in.line_no = 0;
        in.soft_cap = 2;
        in.hard_cap = 2;
        in.max_lines = 4;
        in.min_lines = 1;
        in.operation_id = "orphan-pres-82201";
        ResolveOrCreateResult out;
        if (!PlacementStore::Instance().ResolveOrCreate(in, &out) || !out.ok)
            return Fail("orphan reserve");
        if (PlacementStore::Instance().Occupancy(out.placement.map_instance_id) != 1)
            return Fail("orphan occ");
        const size_t n = PlacementStore::Instance().ReclaimStaleReservations(
            [](uint64_t pid) { return pid != 82201; }, 16);
        if (n < 1)
            return Fail("orphan reclaim");
        if (PlacementStore::Instance().Occupancy(out.placement.map_instance_id) != 0)
            return Fail("orphan occ after reclaim");
    }
    {
        const uint64_t stpl = tpl + 13;
        auto sin = [&](uint64_t player) {
            ResolveOrCreateInput in;
            in.realm_id = 1;
            in.map_template_id = stpl;
            in.player_id = player;
            in.kind = "LINE";
            in.soft_cap = 2;
            in.hard_cap = 2;
            in.max_lines = 8;
            in.min_lines = 1;
            in.operation_id = "stale-l1-" + std::to_string(player);
            return in;
        };
        ResolveOrCreateResult a;
        if (!PlacementStore::Instance().ResolveOrCreate(sin(82301), &a) || !a.ok)
            return Fail("stale seed");
        if (a.placement.line_no != 1)
            return Fail("stale seed line");
        PlacementStore::Instance().ReleaseByPlayer(82301);
        auto rlease = RedisPool::Instance().Acquire();
        if (!rlease)
            return Fail("stale redis");
        const std::string ikey =
            prefix + "map:inst:" + std::to_string(a.placement.map_instance_id);
        const std::string lkey = prefix + "map:line:1:" + std::to_string(stpl) + ":1";
        std::vector<std::string> planted;
        if (!rlease->Eval("redis.call('HSET', KEYS[1], 'state', 'CLOSED') "
                          "redis.call('SET', KEYS[2], ARGV[1]) return 1",
                          {ikey, lkey}, {std::to_string(a.placement.map_instance_id)},
                          &planted))
            return Fail("stale plant");
        rlease = RedisPool::Lease();
        ResolveOrCreateResult b;
        if (!PlacementStore::Instance().ResolveOrCreate(sin(82302), &b) || !b.ok)
            return Fail("stale join");
        if (b.placement.line_no != 1)
            return Fail("stale closed line 1 must not skip to line 2");
    }

    std::printf("OK map_dungeon_test create/member/empty-close/line-min\n");
    std::printf("PASS map_dungeon_test\n");
    return 0;
}
