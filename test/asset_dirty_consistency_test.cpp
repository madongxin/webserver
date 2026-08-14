/**
 * dirty 资产不得被相等/更旧 Apply 清除；重载失败 fail-closed；成功后才恢复写入。
 */
#include "GameLogic.h"
#include "Logging.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

int main() {
    Logger::setLogLevel(Logger::WARN);
    auto &gl = GameLogic::Instance();
    const uint64_t pid = 77001;
    std::map<uint32_t, uint32_t> bag{{1, 10}};
    std::map<uint32_t, int64_t> cds;
    if (!gl.ImportRuntimeState(pid, bag, cds, 5)) {
        std::printf("FAIL import\n");
        return 1;
    }

    gl.SetAssetApplyBlockedForTest(true);
    gl.SetAssetReloadBlockedForTest(true);
    std::vector<GameDbGrantedItem> grants;
    GameDbGrantedItem g;
    g.asset_id = 1;
    g.count = 1;
    grants.push_back(g);
    if (gl.ApplyItemRewardsWithVersion(pid, grants, 6)) {
        std::printf("FAIL apply should fail when blocked\n");
        return 1;
    }
    if (!gl.IsAssetDirty(pid)) {
        std::printf("FAIL expected dirty after blocked apply\n");
        return 1;
    }
    gl.SetAssetApplyBlockedForTest(false);

    if (gl.ApplyItemRewardsWithVersion(pid, grants, 6)) {
        std::printf("FAIL equal committed must not clear dirty without reload\n");
        return 1;
    }
    if (!gl.IsAssetDirty(pid)) {
        std::printf("FAIL dirty cleared by equal version\n");
        return 1;
    }
    if (gl.ApplyItemRewardsWithVersion(pid, grants, 4)) {
        std::printf("FAIL older committed must not clear dirty\n");
        return 1;
    }
    if (!gl.IsAssetDirty(pid)) {
        std::printf("FAIL dirty cleared by older version\n");
        return 1;
    }

    game::ConsumeItemReq creq;
    creq.set_player_id(pid);
    creq.set_item_id(1);
    creq.set_count(1);
    game::GameResponse crsp;
    gl.HandleConsumeItemForTest(creq, &crsp);
    if (crsp.ok() || crsp.message() != "STATE_SYNC_REQUIRED") {
        std::printf("FAIL consume dirty message=%s\n", crsp.message().c_str());
        return 1;
    }
    ::setenv("GAMEMESH_ALLOW_UNSAFE_DEBUG_COMMANDS", "1", 1);
    game::GrantItemReq greq;
    greq.set_player_id(pid);
    greq.set_item_id(1);
    greq.set_count(1);
    game::GameResponse grsp;
    gl.HandleGrantItemForTest(greq, &grsp);
    if (grsp.ok() || grsp.message() != "STATE_SYNC_REQUIRED") {
        std::printf("FAIL grant dirty message=%s\n", grsp.message().c_str());
        return 1;
    }

    gl.SetAssetReloadBlockedForTest(false);
    std::map<uint32_t, uint32_t> db_bag{{1, 11}, {2, 3}};
    gl.SetAssetReloadOverrideForTest(db_bag, 6);
    if (!gl.ApplyItemRewardsWithVersion(pid, grants, 6)) {
        std::printf("FAIL reload apply\n");
        return 1;
    }
    if (gl.IsAssetDirty(pid)) {
        std::printf("FAIL still dirty after reload\n");
        return 1;
    }
    if (gl.GetAssetVersion(pid) != 6 || gl.GetItemCount(pid, 1) != 11 ||
        gl.GetItemCount(pid, 2) != 3) {
        std::printf("FAIL inventory not from GameDB\n");
        return 1;
    }

    game::GameResponse crsp2;
    if (!gl.HandleConsumeItemForTest(creq, &crsp2) || !crsp2.ok()) {
        std::printf("FAIL consume after reload\n");
        return 1;
    }
    gl.ClearAssetReloadOverrideForTest();
    std::printf("OK asset_dirty_consistency_test\n");
    return 0;
}
