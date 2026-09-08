/**
 * P1 LINE：指定线满员不换线、系统选线开新线、并发无重复线号、LEGACY 池仍可用。
 */
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <thread>
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

ResolveOrCreateInput LineIn(uint64_t tpl, uint64_t player, uint32_t line_no = 0,
                            uint64_t inst = 0) {
    ResolveOrCreateInput in;
    in.realm_id = 1;
    in.map_template_id = tpl;
    in.map_instance_id = inst;
    in.player_id = player;
    in.kind = "LINE";
    in.line_no = line_no;
    in.soft_cap = 2;
    in.hard_cap = 2;
    in.max_lines = 8;
    in.operation_id = "line-" + std::to_string(player) + "-" + std::to_string(tpl);
    return in;
}

}  // namespace

int main() {
    if (!InitRedisPoolFromCnf()) {
        std::printf("FAIL map_line_test (Redis unavailable)\n");
        return 1;
    }
    const std::string prefix =
        "gamemesh:test:map_line:" + std::to_string(static_cast<unsigned long long>(::getpid())) +
        ":";
    if (!PlacementStore::Instance().InitFromSessionPrefix(prefix))
        return Fail("PlacementStore init");
    PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});

    const uint64_t tpl = 1101000ULL + static_cast<uint64_t>(::getpid() % 100000);

    // 1. 指定线满员第三人：ERR_MAP_LINE_FULL，第三人 instance 不变（未占位）
    {
        ResolveOrCreateResult a, b, c;
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl, 91001), &a) || !a.ok)
            return Fail("line1 p1");
        if (a.placement.line_no != 1 || a.placement.kind != "LINE")
            return Fail("line1 first line_no/kind");
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl, 91002, 1), &b) || !b.ok)
            return Fail("line1 p2");
        if (b.placement.map_instance_id != a.placement.map_instance_id)
            return Fail("line1 p2 instance");
        if (PlacementStore::Instance().ResolveOrCreate(LineIn(tpl, 91003, 1), &c) || c.ok)
            return Fail("line1 p3 should fail");
        if (c.error_code != "ERR_MAP_LINE_FULL")
            return Fail("line1 p3 code");
        if (c.placement.map_instance_id != 0)
            return Fail("line1 p3 must not switch instance");
    }

    // 2. 系统选线 soft=2：第三人进新线
    {
        const uint64_t tpl2 = tpl + 1;
        ResolveOrCreateResult a, b, c;
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl2, 92001), &a) || !a.ok)
            return Fail("auto p1");
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl2, 92002), &b) || !b.ok)
            return Fail("auto p2");
        if (b.placement.line_no != a.placement.line_no)
            return Fail("auto p2 same line");
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl2, 92003), &c) || !c.ok)
            return Fail("auto p3");
        if (c.placement.line_no == a.placement.line_no)
            return Fail("auto p3 must new line");
        if (c.placement.map_instance_id == a.placement.map_instance_id)
            return Fail("auto p3 instance");
        std::vector<MapLineInfo> lines;
        if (!PlacementStore::Instance().ListLines(1, tpl2, &lines) || lines.size() != 2)
            return Fail("auto list 2 lines");
    }

    // 2b. 系统选线但回传已满软顶的 instance：仍开新线（1000 人 / soft200 = 5 线）
    {
        const uint64_t tpl2b = tpl + 2;
        ResolveOrCreateResult a, b, c;
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl2b, 92101), &a) || !a.ok)
            return Fail("echo p1");
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl2b, 92102, 0, a.placement.map_instance_id),
                                                        &b) ||
            !b.ok)
            return Fail("echo p2");
        if (b.placement.line_no != a.placement.line_no)
            return Fail("echo p2 same line");
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl2b, 92103, 0, a.placement.map_instance_id),
                                                        &c) ||
            !c.ok)
            return Fail("echo p3");
        if (c.placement.line_no == a.placement.line_no)
            return Fail("echo p3 must new line");
        std::vector<MapLineInfo> lines;
        if (!PlacementStore::Instance().ListLines(1, tpl2b, &lines) || lines.size() != 2)
            return Fail("echo list 2 lines");
        // 10 人 soft=2 且每人都回传第一条线 instance → 5 条线
        const uint64_t tpl5 = tpl + 21;
        std::set<uint32_t> five;
        uint64_t echo_inst = 0;
        for (int i = 0; i < 10; ++i) {
            ResolveOrCreateResult out;
            auto in = LineIn(tpl5, 92200ULL + static_cast<uint64_t>(i), 0, echo_inst);
            if (!PlacementStore::Instance().ResolveOrCreate(in, &out) || !out.ok)
                return Fail("five join");
            if (echo_inst == 0)
                echo_inst = out.placement.map_instance_id;
            five.insert(out.placement.line_no);
        }
        if (five.size() != 5)
            return Fail("ten players soft=2 must be 5 lines");
    }

    // 3. 指定不存在的线
    {
        ResolveOrCreateResult out;
        if (PlacementStore::Instance().ResolveOrCreate(LineIn(tpl, 93001, 99), &out) || out.ok)
            return Fail("no line should fail");
        if (out.error_code != "ERR_MAP_NO_LINE")
            return Fail("no line code");
    }

    // 4. 并发开线：无重复 line_no，不超 hard
    {
        const uint64_t tpl3 = tpl + 3;
        constexpr int kN = 16;
        std::atomic<int> ok{0};
        std::vector<uint32_t> lines(static_cast<size_t>(kN), 0);
        std::vector<uint64_t> insts(static_cast<size_t>(kN), 0);
        std::vector<std::thread> th;
        for (int i = 0; i < kN; ++i) {
            th.emplace_back([&, i]() {
                ResolveOrCreateResult out;
                auto in = LineIn(tpl3, 94000ULL + static_cast<uint64_t>(i));
                in.soft_cap = 2;
                in.hard_cap = 2;
                in.max_lines = 16;
                if (PlacementStore::Instance().ResolveOrCreate(in, &out) && out.ok) {
                    lines[static_cast<size_t>(i)] = out.placement.line_no;
                    insts[static_cast<size_t>(i)] = out.placement.map_instance_id;
                    ok.fetch_add(1);
                }
            });
        }
        for (auto &t : th)
            t.join();
        if (ok.load() != kN)
            return Fail("concurrent count");
        std::set<uint32_t> uniq_lines;
        std::set<uint64_t> uniq_inst;
        for (int i = 0; i < kN; ++i) {
            if (lines[static_cast<size_t>(i)] == 0)
                return Fail("concurrent empty line");
            uniq_lines.insert(lines[static_cast<size_t>(i)]);
            uniq_inst.insert(insts[static_cast<size_t>(i)]);
        }
        if (uniq_lines.size() != uniq_inst.size())
            return Fail("concurrent line/inst mismatch");
        if (uniq_lines.size() != static_cast<size_t>(kN / 2))
            return Fail("concurrent line count");
        for (uint64_t id : uniq_inst) {
            if (PlacementStore::Instance().Occupancy(id) > 2)
                return Fail("concurrent over hard");
        }
    }

    // 5. LEGACY_POOL EnterMap(0) 仍是 50 人池
    {
        const uint64_t tpl4 = tpl + 4;
        ResolveOrCreateResult a, b;
        ResolveOrCreateInput in;
        in.realm_id = 1;
        in.map_template_id = tpl4;
        in.player_id = 95001;
        in.capacity = 50;
        in.operation_id = "legacy-a";
        if (!PlacementStore::Instance().ResolveOrCreate(in, &a) || !a.ok)
            return Fail("legacy a");
        in.player_id = 95002;
        in.operation_id = "legacy-b";
        if (!PlacementStore::Instance().ResolveOrCreate(in, &b) || !b.ok)
            return Fail("legacy b");
        if (a.placement.map_instance_id != b.placement.map_instance_id)
            return Fail("legacy same pool");
        if (a.placement.kind == "LINE")
            return Fail("legacy not LINE");
        std::vector<MapLineInfo> lines;
        if (!PlacementStore::Instance().ListLines(1, tpl4, &lines))
            return Fail("legacy list");
        if (!lines.empty())
            return Fail("legacy must not write map:lines");
    }

    // 6. DUNGEON instance=0 禁止
    {
        ResolveOrCreateInput in;
        in.realm_id = 1;
        in.map_template_id = tpl + 5;
        in.player_id = 96001;
        in.kind = "DUNGEON";
        ResolveOrCreateResult out;
        if (PlacementStore::Instance().ResolveOrCreate(in, &out) || out.ok)
            return Fail("dungeon 0 should fail");
        if (out.error_code != "ERR_DUNGEON_CREATE_FORBIDDEN")
            return Fail("dungeon 0 code");
    }

    // 7. 空线 lease 过期后系统选线仍进 1 线（不新开 2 线）
    {
        const uint64_t tpl7 = tpl + 7;
        ResolveOrCreateResult a, b;
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl7, 97001), &a) || !a.ok)
            return Fail("lease p1");
        if (a.placement.line_no != 1)
            return Fail("lease p1 line");
        PlacementStore::Instance().ReleaseByPlayer(97001);
        auto rlease = RedisPool::Instance().Acquire();
        if (!rlease)
            return Fail("lease redis");
        const std::string key =
            prefix + "map:inst:" + std::to_string(a.placement.map_instance_id);
        std::vector<std::string> reply;
        if (!rlease->Eval("redis.call('HSET', KEYS[1], 'leaseUntil', '1') return 1", {key}, {},
                          &reply))
            return Fail("lease expire hset");
        if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl7, 97002), &b) || !b.ok)
            return Fail("lease p2");
        if (b.placement.line_no != 1)
            return Fail("expired line must stay line 1");
        if (b.placement.map_instance_id != a.placement.map_instance_id)
            return Fail("expired line must reuse instance");
    }

    std::printf("OK map_line_test specified/auto/concurrent/legacy/lease-reuse\n");
    std::printf("PASS map_line_test\n");
    return 0;
}
