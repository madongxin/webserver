/**
 * FullSnapshot 序列 + Replay 空洞检测 + ACK gap + 真实 Protobuf baseline
 */
#include "PushReplayStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include "game.pb.h"

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
        std::printf("ERROR: Redis required for push_full_snapshot_test\n");
        return 1;
    }
    const std::string prefix =
        "gamemesh:dev:pushsnap:" + std::to_string(static_cast<long long>(::getpid())) + ":";
    if (!PushReplayStore::Instance().InitFromSessionPrefix(prefix, 64, 600))
        return Fail("init");

    const uint64_t player = 880001;
    const std::string sid = "snap-sess";
    PushReplayStore::Instance().InvalidateSession(player, sid);

    // 1) Reserve 1 不 Append，再写 seq2 → ReplayAfter(0) NEED_SNAPSHOT
    {
        const uint64_t p = player + 1;
        const std::string s = sid + "-gap1";
        PushReplayStore::Instance().InvalidateSession(p, s);
        if (PushReplayStore::Instance().ReserveSeq(p, s) != 1)
            return Fail("reserve1");
        if (PushReplayStore::Instance().AppendReliable(p, s, "push", "body2") != 2)
            return Fail("append2");
        std::vector<PushReplayEntry> out;
        bool need = false;
        if (PushReplayStore::Instance().ReplayAfter(p, s, 0, &out, &need) || !need)
            return Fail("reserve hole need snapshot");
    }

    // 2) seq 1、3 存在，缺 2 → ReplayAfter(1) NEED_SNAPSHOT
    {
        const uint64_t p = player + 2;
        const std::string s = sid + "-gap2";
        PushReplayStore::Instance().InvalidateSession(p, s);
        if (PushReplayStore::Instance().AppendReliable(p, s, "push", "a") != 1)
            return Fail("seq1");
        if (PushReplayStore::Instance().ReserveSeq(p, s) != 2)
            return Fail("reserve2");
        if (PushReplayStore::Instance().AppendReliable(p, s, "push", "c") != 3)
            return Fail("seq3");
        std::vector<PushReplayEntry> out;
        bool need = false;
        if (PushReplayStore::Instance().ReplayAfter(p, s, 1, &out, &need) || !need)
            return Fail("mid gap need snapshot");
    }

    // 3) Reserve 与普通 Push 并发：不得静默漏消息（最终要么连续可回放，要么 NEED_SNAPSHOT）
    {
        const uint64_t p = player + 3;
        const std::string s = sid + "-race";
        PushReplayStore::Instance().InvalidateSession(p, s);
        std::atomic<uint64_t> reserved{0};
        std::thread t1([&]() {
            reserved.store(PushReplayStore::Instance().ReserveSeq(p, s));
        });
        std::thread t2([&]() {
            PushReplayStore::Instance().AppendReliable(p, s, "push", "race");
        });
        t1.join();
        t2.join();
        const uint64_t rseq = reserved.load();
        if (rseq > 0) {
            game::GameResponse inner;
            auto *fs = inner.mutable_full_snapshot();
            fs->set_ok(true);
            fs->set_player_id(p);
            fs->set_baseline_server_seq(rseq);
            std::string payload;
            inner.SerializeToString(&payload);
            (void)PushReplayStore::Instance().AppendReserved(p, s, rseq, "full_snapshot", payload);
        }
        std::vector<PushReplayEntry> out;
        bool need = false;
        const bool ok = PushReplayStore::Instance().ReplayAfter(p, s, 0, &out, &need);
        if (!ok && !need)
            return Fail("race replay hard fail");
        if (ok) {
            uint64_t expect = 1;
            for (const auto &e : out) {
                if (e.server_seq != expect)
                    return Fail("race silent gap");
                ++expect;
            }
        }
    }

    // 4) 重复 AppendReserved 不产生重复条目
    {
        const uint64_t p = player + 4;
        const std::string s = sid + "-dup";
        PushReplayStore::Instance().InvalidateSession(p, s);
        const uint64_t seq = PushReplayStore::Instance().ReserveSeq(p, s);
        if (!PushReplayStore::Instance().AppendReserved(p, s, seq, "full_snapshot", "once"))
            return Fail("append reserved");
        if (!PushReplayStore::Instance().AppendReserved(p, s, seq, "full_snapshot", "once"))
            return Fail("dup append should idempotent ok");
        std::vector<PushReplayEntry> out;
        bool need = false;
        if (!PushReplayStore::Instance().ReplayAfter(p, s, 0, &out, &need) || need || out.size() != 1)
            return Fail("dup append list size");
    }

    // 5) ACK reserved-but-not-stored 被拒绝
    {
        const uint64_t p = player + 5;
        const std::string s = sid + "-ackgap";
        PushReplayStore::Instance().InvalidateSession(p, s);
        if (PushReplayStore::Instance().ReserveSeq(p, s) != 1)
            return Fail("ack reserve");
        auto ar = PushReplayStore::Instance().Ack(p, s, 1);
        if (ar.ok() || ar.status != PushReplayStore::AckStatus::Gap)
            return Fail("ack gap rejected");
        if (ar.error_code != "NEED_SNAPSHOT" && ar.error_code != "ERR_ACK_GAP")
            return Fail("ack gap code");
    }

    // 5b) 中间空洞：有 1、3 缺 2 时 ACK 3 必须拒绝（不得跳过）
    {
        const uint64_t p = player + 55;
        const std::string s = sid + "-ackmid";
        PushReplayStore::Instance().InvalidateSession(p, s);
        if (PushReplayStore::Instance().AppendReliable(p, s, "t", "a") != 1)
            return Fail("mid append1");
        if (PushReplayStore::Instance().ReserveSeq(p, s) != 2)
            return Fail("mid reserve2");
        if (PushReplayStore::Instance().ReserveSeq(p, s) != 3)
            return Fail("mid reserve3");
        if (!PushReplayStore::Instance().AppendReserved(p, s, 3, "t", "c"))
            return Fail("mid append3");
        auto ar = PushReplayStore::Instance().Ack(p, s, 3);
        if (ar.ok() || ar.status != PushReplayStore::AckStatus::Gap)
            return Fail("mid ack skip rejected");
        auto a1 = PushReplayStore::Instance().Ack(p, s, 1);
        if (!a1.ok())
            return Fail("mid ack1");
    }

    // 6) 真实 FullSnapshot protobuf：envelope seq / baseline / replay 一致
    {
        const uint64_t p = player + 6;
        const std::string s = sid + "-pb";
        PushReplayStore::Instance().InvalidateSession(p, s);
        const uint64_t seq = PushReplayStore::Instance().ReserveSeq(p, s);
        if (seq == 0)
            return Fail("pb reserve");

        game::GameResponse inner;
        auto *fs = inner.mutable_full_snapshot();
        fs->set_ok(true);
        fs->set_player_id(p);
        fs->set_asset_version(7);
        fs->set_baseline_server_seq(seq);
        fs->add_item_ids(1001);
        fs->add_item_counts(3);
        std::string payload;
        if (!inner.SerializeToString(&payload))
            return Fail("serialize");

        game::ServerPushEnvelope env;
        env.set_server_seq(seq);
        env.set_message_type("full_snapshot");
        env.set_payload(payload);
        env.set_reliable(true);

        if (!PushReplayStore::Instance().AppendReserved(p, s, seq, "full_snapshot", payload))
            return Fail("pb append");

        std::vector<PushReplayEntry> out;
        bool need = false;
        if (!PushReplayStore::Instance().ReplayAfter(p, s, 0, &out, &need) || need || out.size() != 1)
            return Fail("pb replay");
        if (out[0].server_seq != seq || out[0].server_seq != env.server_seq())
            return Fail("pb seq mismatch");

        game::GameResponse decoded;
        if (!decoded.ParseFromString(out[0].payload) || !decoded.has_full_snapshot())
            return Fail("pb parse");
        if (decoded.full_snapshot().baseline_server_seq() != seq)
            return Fail("pb baseline mismatch");
    }

    std::printf("OK push_full_snapshot_test\n");
    return 0;
}
