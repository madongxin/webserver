/**
 * 阶段二：运行时导出/导入幂等 + checksum
 */
#include "GameLogic.h"
#include "game.pb.h"

#include <cstdio>
#include <map>
#include <string>

namespace {

int g_fail = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL %s\n", m);
        ++g_fail;
    } else {
        std::printf("ok %s\n", m);
    }
}

}  // namespace

int main() {
    auto &gl = GameLogic::Instance();
    const uint64_t pid = 88001;
    {
        std::map<uint32_t, uint32_t> bag{{1001, 7}, {1002, 3}};
        std::map<uint32_t, int64_t> cds{{9, 12345}};
        Expect(gl.ImportRuntimeState(pid, bag, cds, 42), "import seed");
    }
    std::map<uint32_t, uint32_t> bag2;
    std::map<uint32_t, int64_t> cds2;
    uint64_t ver = 0;
    Expect(gl.ExportRuntimeState(pid, &bag2, &cds2, &ver), "export");
    Expect(bag2[1001] == 7 && bag2[1002] == 3, "bag preserved");
    Expect(cds2[9] == 12345, "skill cd preserved");
    Expect(ver == 42, "asset version");

    game::FullStateSnapshotRsp snap;
    Expect(gl.BuildFullStateSnapshot(pid, &snap) && snap.ok(), "full snapshot");
    Expect(snap.item_ids_size() == 2, "snapshot items");
    Expect(snap.asset_version() == 42, "snapshot asset ver");

    // 覆盖导入
    std::map<uint32_t, uint32_t> bag3{{1001, 1}};
    Expect(gl.ImportRuntimeState(pid, bag3, {}, 43), "reimport");
    bag2.clear();
    Expect(gl.ExportRuntimeState(pid, &bag2, &cds2, &ver) && bag2[1001] == 1 && ver == 43,
           "overwrite");

    if (g_fail) {
        std::printf("FAIL phase2_transfer_snapshot_test fails=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS phase2_transfer_snapshot_test\n");
    return 0;
}
