/**
 * @file map_placement_test.cpp
 * @brief 阶段 5：Placement RR、多图同 Logic、epoch 迁移后旧 Owner 拒写
 */

#include "Logging.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_fail = 0;

#define EXPECT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    MapPlacement::Instance().ClearForTest();
    MapInstanceRegistry::Instance().ClearForTest();
    MapPlacement::Instance().ConfigureOwners({"gl-0", "gl-1"});

    // A: RR 分配两个不同模板到不同 Logic
    MapPlacementRecord a, b;
    EXPECT_TRUE(MapPlacement::Instance().ResolveOrAllocate(1, 1001, 0, &a));
    EXPECT_TRUE(MapPlacement::Instance().ResolveOrAllocate(1, 2002, 0, &b));
    EXPECT_TRUE(a.map_instance_id != b.map_instance_id);
    EXPECT_TRUE(a.owner_gamelogic_id == "gl-0");
    EXPECT_TRUE(b.owner_gamelogic_id == "gl-1");
    EXPECT_TRUE(a.owner_epoch == 1);

    // B: 同一 Logic 承载两个实例
    MapInstanceRegistry::Instance().SetLocalInstanceId("gl-0");
    EXPECT_TRUE(MapInstanceRegistry::Instance().Claim(a.map_instance_id, a.map_template_id, 1));
    MapPlacementRecord a2;
    EXPECT_TRUE(MapPlacement::Instance().ResolveOrAllocate(1, 1001, 0, &a2));
    EXPECT_TRUE(a2.owner_gamelogic_id == "gl-0");
    EXPECT_TRUE(MapInstanceRegistry::Instance().Claim(a2.map_instance_id, a2.map_template_id,
                                                     a2.owner_epoch));
    EXPECT_TRUE(MapInstanceRegistry::Instance().Has(a.map_instance_id));
    EXPECT_TRUE(MapInstanceRegistry::Instance().Has(a2.map_instance_id));
    EXPECT_TRUE(MapInstanceRegistry::Instance().AddPlayer(a.map_instance_id, 42));
    EXPECT_TRUE(MapInstanceRegistry::Instance().PlayerCount(a.map_instance_id) == 1);

    // C: epoch 栅栏 + Migrate 后旧 Owner 不能写
    EXPECT_TRUE(MapInstanceRegistry::Instance().AcceptWrite(a.map_instance_id, 1));
    EXPECT_TRUE(!MapInstanceRegistry::Instance().AcceptWrite(a.map_instance_id, 2));

    MapPlacementRecord migrated;
    EXPECT_TRUE(MapPlacement::Instance().Migrate(a.map_instance_id, "gl-1", &migrated));
    EXPECT_TRUE(migrated.owner_epoch == 2);
    EXPECT_TRUE(migrated.owner_gamelogic_id == "gl-1");
    EXPECT_TRUE(migrated.route_version == 2);

    EXPECT_TRUE(!MapInstanceRegistry::Instance().AcceptWrite(a.map_instance_id, 2));
    MapInstanceRegistry::Instance().Release(a.map_instance_id);
    MapInstanceRegistry::Instance().SetLocalInstanceId("gl-1");
    EXPECT_TRUE(MapInstanceRegistry::Instance().Claim(a.map_instance_id, a.map_template_id, 2));
    EXPECT_TRUE(MapInstanceRegistry::Instance().AcceptWrite(a.map_instance_id, 2));
    EXPECT_TRUE(!MapInstanceRegistry::Instance().AcceptWrite(a.map_instance_id, 1));

    // D: 更低 epoch Claim 被拒
    EXPECT_TRUE(!MapInstanceRegistry::Instance().Claim(a.map_instance_id, a.map_template_id, 1));

    if (g_fail) {
        std::fprintf(stderr, "FAILED %d\n", g_fail);
        std::_Exit(1);
    }
    std::printf("OK map_placement_test\n");
    std::fflush(stdout);
    std::_Exit(0);
}
