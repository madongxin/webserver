/**
 * 为 E2E 预创建指定 Owner 的地图实例。
 * 用法: placement_seed_tool <map_template_id> <owner_logic_id> [realm_id]
 * 输出: map_instance_id=... owner=... epoch=...
 */
#include "PlacementStore.h"
#include "SessionStore.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

bool InitRedis() { return SessionStore::Instance().InitFromConfig(); }

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <map_template_id> <owner> [realm]\n", argv[0]);
        return 2;
    }
    if (!InitRedis()) {
        std::fprintf(stderr, "redis init failed\n");
        return 3;
    }
    PlacementStore::Instance().InitFromSessionPrefix(SessionStore::Instance().key_prefix());
    PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1", "gl-2"});

    ResolveOrCreateInput in;
    in.map_template_id = std::strtoull(argv[1], nullptr, 10);
    in.preferred_owner = argv[2];
    in.realm_id = argc >= 4 ? static_cast<uint32_t>(std::atoi(argv[3])) : 1;
    in.force_new = true;
    ResolveOrCreateResult out;
    if (!PlacementStore::Instance().ResolveOrCreate(in, &out) || !out.ok) {
        std::fprintf(stderr, "seed failed: %s\n", out.message.c_str());
        return 4;
    }
    std::printf("map_instance_id=%llu\n",
                static_cast<unsigned long long>(out.placement.map_instance_id));
    std::printf("owner=%s\n", out.placement.owner_logic_server_id.c_str());
    std::printf("owner_epoch=%llu\n",
                static_cast<unsigned long long>(out.placement.owner_epoch));
    std::printf("map_template_id=%llu\n",
                static_cast<unsigned long long>(out.placement.map_template_id));
    return 0;
}
