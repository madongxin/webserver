/**
 * 阶段一：FullSnapshot 失败语义 — 导出失败不得当作成功 baseline
 * （轻量：验证 PushReplayStore 在 Append 失败时 seq=0，调用方不得宣称成功）
 */
#include "PushReplayStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
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

}  // namespace

int main() {
    if (!InitRedis()) {
        std::printf("ERROR: Redis required for push_full_snapshot_test\n");
        return 1;
    }
    const std::string prefix =
        "gamemesh:dev:pushsnap:" + std::to_string(static_cast<long long>(::getpid())) + ":";
    if (!PushReplayStore::Instance().InitFromSessionPrefix(prefix, 8, 600)) {
        std::printf("FAIL init\n");
        return 1;
    }
    // 无效参数 Append → seq=0，模拟 store 失败路径：不得当作 baseline 成功
    const uint64_t bad = PushReplayStore::Instance().AppendReliable(0, "s", "full_snapshot", "x");
    if (bad != 0) {
        std::printf("FAIL expect seq=0 on invalid append\n");
        return 1;
    }
    const uint64_t player = 880001;
    const std::string sid = "snap-sess";
    PushReplayStore::Instance().InvalidateSession(player, sid);
    const std::string payload = "full-snapshot-body";
    const uint64_t seq =
        PushReplayStore::Instance().AppendReliable(player, sid, "full_snapshot", payload);
    if (seq == 0) {
        std::printf("FAIL append snapshot\n");
        return 1;
    }
    std::vector<PushReplayEntry> out;
    bool need = false;
    if (!PushReplayStore::Instance().ReplayAfter(player, sid, 0, &out, &need) || out.empty()) {
        std::printf("FAIL replay snapshot\n");
        return 1;
    }
    if (out[0].server_seq != seq || out[0].message_type != "full_snapshot" ||
        out[0].payload != payload) {
        std::printf("FAIL snapshot envelope mismatch\n");
        return 1;
    }
    std::printf("OK push_full_snapshot_test seq=%llu\n", (unsigned long long)seq);
    return 0;
}
