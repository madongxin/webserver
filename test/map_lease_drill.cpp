/**
 * 阶段 4 演练辅助：MarkRecovering + Migrate，校验 epoch 递增。
 * 用法：map_lease_drill <map_id> <new_owner> [expect_old_epoch]
 */
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

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

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <map_id> <new_owner> [expect_old_epoch]\n", argv[0]);
        return 2;
    }
    const uint64_t map_id = std::strtoull(argv[1], nullptr, 10);
    const std::string new_owner = argv[2];
    const uint64_t expect_old = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0;
    if (!InitRedis() || !PlacementStore::Instance().InitFromSessionPrefix("gamemesh:dev:")) {
        std::fprintf(stderr, "redis/placement init failed\n");
        return 1;
    }
    PlacementRecord before;
    if (!PlacementStore::Instance().Get(map_id, &before)) {
        std::fprintf(stderr, "map not found\n");
        return 1;
    }
    if (expect_old != 0 && before.owner_epoch != expect_old) {
        std::fprintf(stderr, "epoch mismatch got=%llu expect=%llu\n",
                     (unsigned long long)before.owner_epoch, (unsigned long long)expect_old);
        return 1;
    }
    PlacementRecord rec;
    if (!PlacementStore::Instance().MarkRecovering(map_id, "kill_logic_drill", &rec)) {
        std::fprintf(stderr, "MarkRecovering failed\n");
        return 1;
    }
    PlacementRecord mig;
    std::string err;
    if (!PlacementStore::Instance().Migrate(map_id, new_owner, before.owner_epoch, "kill-drill",
                                            &mig, &err)) {
        std::fprintf(stderr, "Migrate failed: %s\n", err.c_str());
        return 1;
    }
    if (mig.owner_epoch <= before.owner_epoch || mig.owner_logic_server_id != new_owner) {
        std::fprintf(stderr, "migrate result invalid epoch=%llu owner=%s\n",
                     (unsigned long long)mig.owner_epoch, mig.owner_logic_server_id.c_str());
        return 1;
    }
    // 旧 epoch Heartbeat 必须失败
    int64_t until = 0;
    if (PlacementStore::Instance().Heartbeat(map_id, before.owner_logic_server_id,
                                             before.owner_epoch, 30, &until)) {
        std::fprintf(stderr, "old owner heartbeat should fail\n");
        return 1;
    }
    std::printf("OK map=%llu %s epoch %llu -> %llu owner=%s\n", (unsigned long long)map_id,
                before.owner_logic_server_id.c_str(), (unsigned long long)before.owner_epoch,
                (unsigned long long)mig.owner_epoch, mig.owner_logic_server_id.c_str());
    return 0;
}
