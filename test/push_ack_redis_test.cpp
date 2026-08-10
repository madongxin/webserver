/**
 * 阶段一：Push ACK 原子校验（ahead / stale / duplicate）
 */
#include "PushReplayStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
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
    std::ifstream in(RedisConfigPath::RedisCnf());
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

int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

}  // namespace

int main() {
    if (!InitRedis()) {
        std::printf("ERROR: Redis required for push_ack_redis_test\n");
        return 1;
    }
    const std::string prefix =
        "gamemesh:dev:pushacktest:" + std::to_string(static_cast<long long>(::getpid())) + ":";
    if (!PushReplayStore::Instance().InitFromSessionPrefix(prefix, 32, 600))
        return Fail("init");

    const uint64_t player = 800001;
    const std::string sid = "sess-ack";
    PushReplayStore::Instance().InvalidateSession(player, sid);

    const uint64_t s1 =
        PushReplayStore::Instance().AppendReliable(player, sid, "t", "a");
    const uint64_t s2 =
        PushReplayStore::Instance().AppendReliable(player, sid, "t", "b");
    if (s1 == 0 || s2 <= s1)
        return Fail("append");

    auto r1 = PushReplayStore::Instance().Ack(player, sid, s1);
    if (!r1.ok() || r1.status != PushReplayStore::AckStatus::Ok)
        return Fail("ack ok");

    auto rdup = PushReplayStore::Instance().Ack(player, sid, s1);
    if (!rdup.ok() || rdup.status != PushReplayStore::AckStatus::Duplicate)
        return Fail("ack duplicate");

    auto r2 = PushReplayStore::Instance().Ack(player, sid, s2);
    if (!r2.ok())
        return Fail("ack s2");

    auto rstale = PushReplayStore::Instance().Ack(player, sid, s1);
    if (rstale.ok() || rstale.status != PushReplayStore::AckStatus::Stale)
        return Fail("ack stale");

    const uint64_t cur = PushReplayStore::Instance().CurrentSeq(player, sid);
    auto rahead = PushReplayStore::Instance().Ack(player, sid, cur + 10);
    if (rahead.ok() || rahead.status != PushReplayStore::AckStatus::Ahead)
        return Fail("ack ahead");

    // 并发乱序：不应回退 lastAck；超前必须失败
    std::atomic<int> ahead_n{0};
    std::atomic<int> ok_n{0};
    std::vector<std::thread> th;
    for (int i = 0; i < 8; ++i) {
        th.emplace_back([&, i]() {
            auto r = PushReplayStore::Instance().Ack(player, sid, s2 + static_cast<uint64_t>(i));
            if (r.status == PushReplayStore::AckStatus::Ahead)
                ahead_n.fetch_add(1);
            if (r.ok())
                ok_n.fetch_add(1);
        });
    }
    for (auto &t : th)
        t.join();
    if (ahead_n.load() < 1)
        return Fail("concurrent ahead");

    // 非法 ACK 后 replay 仍完整（s2 已 ack，list 应空或仅更新 lastAck）
    std::vector<PushReplayEntry> out;
    bool need = false;
    if (!PushReplayStore::Instance().ReplayAfter(player, sid, s2, &out, &need))
        return Fail("replay after");
    if (!out.empty())
        return Fail("replay should be empty after full ack");

    std::printf("OK push_ack_redis_test\n");
    return 0;
}
