/**
 * 阶段 5：Redis PushReplayStore — 原子 seq、回放、ACK 裁剪、缺口 NeedFullSnapshot
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
        std::printf("SKIP push_replay_store_test (Redis unavailable)\n");
        return 0;
    }
    const std::string prefix = "gamemesh:dev:pushreplaytest:";
    if (!PushReplayStore::Instance().InitFromSessionPrefix(prefix, 8, 600))
        return Fail("init");

    const uint64_t player = 700001;
    // 清理旧 key（新 prefix 一般干净）
    const uint64_t s1 =
        PushReplayStore::Instance().AppendReliable(player, "enter_map_notify", "payload-a");
    const uint64_t s2 =
        PushReplayStore::Instance().AppendReliable(player, "enter_map_notify", "payload-b");
    if (s1 == 0 || s2 <= s1)
        return Fail("append seq");

    std::vector<PushReplayEntry> out;
    bool need_snap = false;
    if (!PushReplayStore::Instance().ReplayAfter(player, 0, &out, &need_snap) || need_snap)
        return Fail("replay from 0");
    if (out.size() != 2 || out[0].server_seq != s1 || out[1].payload != "payload-b")
        return Fail("replay content");

    if (!PushReplayStore::Instance().Ack(player, s1))
        return Fail("ack");
    out.clear();
    if (!PushReplayStore::Instance().ReplayAfter(player, s1, &out, &need_snap) || need_snap)
        return Fail("replay after ack");
    if (out.size() != 1 || out[0].server_seq != s2)
        return Fail("after ack one left");

    // 人为造成缺口：再写满并裁剪后用过旧 last_seq
    for (int i = 0; i < 20; ++i)
        PushReplayStore::Instance().AppendReliable(player, "t", "x");
    out.clear();
    need_snap = false;
    if (PushReplayStore::Instance().ReplayAfter(player, 1, &out, &need_snap) || !need_snap)
        return Fail("expect need_snapshot on gap");

    // 单调：CurrentSeq 不回退
    const uint64_t cur = PushReplayStore::Instance().CurrentSeq(player);
    if (cur < s2)
        return Fail("seq went backwards");

    std::printf("OK push_replay_store_test append/replay/ack/gap/seq\n");
    std::printf("PASS push_replay_store_test\n");
    return 0;
}
