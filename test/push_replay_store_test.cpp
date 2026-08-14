/**
 * 阶段二：Redis PushReplayStore — session 隔离、原子 seq、回放、ACK、缺口
 */
#include "PushReplayStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool InitRedis() {
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
    return RedisPool::Instance().Init(host, port, password, 4);
}

int Fail(const char *msg) {
    std::printf("FAIL %s\n", msg);
    return 1;
}

}  // namespace

int main() {
    if (!InitRedis()) {
        std::printf("FAIL push_replay_store_test (Redis unavailable)\n");
        return 1;
    }
    const std::string prefix = "gamemesh:dev:pushreplaytest:";
    if (!PushReplayStore::Instance().InitFromSessionPrefix(prefix, 8, 600))
        return Fail("init");

    const uint64_t player = 700001;
    const std::string sid_a = "sess-a";
    const std::string sid_b = "sess-b";
    PushReplayStore::Instance().InvalidateSession(player, sid_a);
    PushReplayStore::Instance().InvalidateSession(player, sid_b);

    const uint64_t s1 =
        PushReplayStore::Instance().AppendReliable(player, sid_a, "enter_map_notify", "payload-a");
    const uint64_t s2 =
        PushReplayStore::Instance().AppendReliable(player, sid_a, "enter_map_notify", "payload-b");
    if (s1 == 0 || s2 <= s1)
        return Fail("append seq");

    std::vector<PushReplayEntry> out;
    bool need_snap = false;
    if (!PushReplayStore::Instance().ReplayAfter(player, sid_a, 0, &out, &need_snap) || need_snap)
        return Fail("replay from 0");
    if (out.size() != 2 || out[0].server_seq != s1 || out[1].payload != "payload-b")
        return Fail("replay content");

    // 新 Session 看不到旧 Session 消息
    out.clear();
    if (!PushReplayStore::Instance().ReplayAfter(player, sid_b, 0, &out, &need_snap))
        return Fail("replay empty new session");
    if (!out.empty())
        return Fail("session isolation broken");

    if (!PushReplayStore::Instance().Ack(player, sid_a, s1).ok())
        return Fail("ack");
    // 旧 session ACK 不能裁新 session（新 session 无消息且 cur=0 → ahead/invalid）
    (void)PushReplayStore::Instance().Ack(player, sid_b, s1);
    out.clear();
    if (!PushReplayStore::Instance().ReplayAfter(player, sid_a, s1, &out, &need_snap) || need_snap)
        return Fail("replay after ack");
    if (out.size() != 1 || out[0].server_seq != s2)
        return Fail("after ack one left");

    for (int i = 0; i < 20; ++i)
        PushReplayStore::Instance().AppendReliable(player, sid_a, "t", "x");
    out.clear();
    need_snap = false;
    if (PushReplayStore::Instance().ReplayAfter(player, sid_a, 1, &out, &need_snap) || !need_snap)
        return Fail("expect need_snapshot on gap");

    const uint64_t cur = PushReplayStore::Instance().CurrentSeq(player, sid_a);
    if (cur < s2)
        return Fail("seq went backwards");

    std::printf("OK push_replay_store_test session/append/replay/ack/gap/seq\n");
    std::printf("PASS push_replay_store_test\n");
    return 0;
}
