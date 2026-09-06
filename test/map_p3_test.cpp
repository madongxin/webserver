/**
 * P3：SwitchLine 原子迁占位、排队票、DRAINING 拒新进、热迁换 Owner。
 */
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

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
    in.operation_id = "p3-" + std::to_string(player) + "-" + std::to_string(tpl);
    return in;
}

}  // namespace

int main() {
    if (!InitRedisPoolFromCnf()) {
        std::printf("FAIL map_p3_test (Redis unavailable)\n");
        return 1;
    }
    const std::string prefix =
        "gamemesh:test:map_p3:" + std::to_string(static_cast<unsigned long long>(::getpid())) + ":";
    if (!PlacementStore::Instance().InitFromSessionPrefix(prefix))
        return Fail("PlacementStore init");
    PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});

    const uint64_t tpl = 2101000ULL + static_cast<uint64_t>(::getpid() % 100000);

    ResolveOrCreateResult a, b, c;
    if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl, 81001), &a) || !a.ok)
        return Fail("line1 p1");
    if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl, 81002), &b) || !b.ok)
        return Fail("line1 p2");
    if (b.placement.line_no != a.placement.line_no)
        return Fail("same first line");
    if (!PlacementStore::Instance().ResolveOrCreate(LineIn(tpl, 81003), &c) || !c.ok)
        return Fail("auto new line");
    if (c.placement.line_no == a.placement.line_no)
        return Fail("p3 must be new line");

    SwitchLineInput sw;
    sw.realm_id = 1;
    sw.map_template_id = tpl;
    sw.player_id = 81003;
    sw.line_no = a.placement.line_no;
    sw.soft_cap = 2;
    sw.hard_cap = 2;
    sw.operation_id = "switch-full";
    ResolveOrCreateResult swf;
    if (PlacementStore::Instance().SwitchLine(sw, &swf) || swf.ok)
        return Fail("switch onto full line should fail");
    if (swf.error_code != "ERR_MAP_LINE_FULL")
        return Fail("switch full code");

    PlacementStore::Instance().ReleaseByPlayer(81002);
    sw.operation_id = "switch-ok";
    ResolveOrCreateResult swo;
    if (!PlacementStore::Instance().SwitchLine(sw, &swo) || !swo.ok)
        return Fail("switch after slot free");
    if (swo.placement.line_no != a.placement.line_no)
        return Fail("switch target line");
    if (swo.placement.map_instance_id != a.placement.map_instance_id)
        return Fail("switch target instance");
    if (PlacementStore::Instance().Occupancy(c.placement.map_instance_id) != 0)
        return Fail("old line occupancy");

    EnqueueMapInput eq;
    eq.realm_id = 1;
    eq.map_template_id = tpl;
    eq.player_id = 81004;
    eq.line_no = a.placement.line_no;
    eq.hard_cap = 2;
    EnqueueMapResult eqr;
    if (!PlacementStore::Instance().EnqueueMap(eq, &eqr) || !eqr.ok)
        return Fail("enqueue");
    if (eqr.queue_token.empty() || eqr.queue_position != 1)
        return Fail("enqueue token/pos");
    EnqueueMapInput poll = eq;
    poll.queue_token = eqr.queue_token;
    EnqueueMapResult polr;
    if (!PlacementStore::Instance().EnqueueMap(poll, &polr) || !polr.ok)
        return Fail("poll queue");
    if (polr.queue_token != eqr.queue_token)
        return Fail("poll token");

    ResolveOrCreateInput redeem = LineIn(tpl, 81004, a.placement.line_no);
    redeem.queue_token = eqr.queue_token;
    redeem.operation_id = "redeem-not-ready";
    ResolveOrCreateResult rnr;
    if (PlacementStore::Instance().ResolveOrCreate(redeem, &rnr) || rnr.ok)
        return Fail("redeem while full");
    if (rnr.error_code != "ERR_QUEUE_NOT_READY" && rnr.error_code != "ERR_MAP_LINE_FULL")
        return Fail("redeem blocked code");

    PlacementStore::Instance().ReleaseByPlayer(81001);
    redeem.operation_id = "redeem-ok";
    ResolveOrCreateResult rok;
    if (!PlacementStore::Instance().ResolveOrCreate(redeem, &rok) || !rok.ok)
        return Fail("redeem after free");
    if (rok.placement.map_instance_id != a.placement.map_instance_id)
        return Fail("redeem instance");

    PlacementRecord drained;
    std::string derr;
    if (!PlacementStore::Instance().Drain(a.placement.map_instance_id, "test", &drained, &derr))
        return Fail("drain");
    if (drained.state != PlacementState::Draining)
        return Fail("drain state");
    ResolveOrCreateInput join_drain = LineIn(tpl, 81005, a.placement.line_no);
    join_drain.operation_id = "join-drain";
    ResolveOrCreateResult jd;
    if (PlacementStore::Instance().ResolveOrCreate(join_drain, &jd) || jd.ok)
        return Fail("join draining should fail");

    PlacementRecord migrated;
    std::string merr;
    if (!PlacementStore::Instance().Migrate(a.placement.map_instance_id, "gl-1", 0, "mig-p3",
                                            &migrated, &merr))
        return Fail("migrate after drain");
    if (migrated.owner_logic_server_id != "gl-1")
        return Fail("migrate owner");
    if (migrated.owner_epoch <= drained.owner_epoch)
        return Fail("migrate epoch");

    std::vector<uint64_t> occ;
    if (!PlacementStore::Instance().ListOccupants(a.placement.map_instance_id, &occ))
        return Fail("list occupants");
    if (occ.empty())
        return Fail("occupants after migrate");

    std::printf("OK map_p3_test switch/queue/drain/migrate\n");
    std::printf("PASS map_p3_test\n");
    return 0;
}
