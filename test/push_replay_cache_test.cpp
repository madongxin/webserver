#include "PushReplayCache.h"

#include <cstdio>

static int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

int main() {
    auto &c = PushReplayCache::Instance();
    c.Configure(3);
    c.ClearPlayer(1);

    const uint64_t s1 = c.NextSeq(1);
    const uint64_t s2 = c.NextSeq(1);
    const uint64_t s3 = c.NextSeq(1);
    PushReplayEntry e;
    e.reliable = true;
    e.message_type = "t";
    e.payload = "p1";
    e.server_seq = s1;
    c.Store(1, e);
    e.server_seq = s2;
    e.payload = "p2";
    c.Store(1, e);
    e.server_seq = s3;
    e.payload = "p3";
    c.Store(1, e);
    // 超出 cap，最旧被挤出
    const uint64_t s4 = c.NextSeq(1);
    e.server_seq = s4;
    e.payload = "p4";
    c.Store(1, e);

    std::vector<PushReplayEntry> out;
    bool need_snap = false;
    if (!c.ReplayAfter(1, s1, &out, &need_snap) || need_snap)
        return Fail("gap should need snapshot after eviction");

    if (!c.ReplayAfter(1, s2, &out, &need_snap) || need_snap)
        return Fail("replay after s2");
    if (out.size() < 2)
        return Fail("replay size");

    std::printf("PASS push_replay_cache_test\n");
    return 0;
}
