#include "Logging.h"
#include "MapRuntime.h"
#include "MapStaticData.h"
#include "ServerStats.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

std::shared_ptr<const MapStaticData> MakeMap() {
    const std::string json = R"JSON({
      "schema_version": 1, "map_template_id": 9102, "scene_name": "Crowd", "data_version": 1,
      "bounds_min": [0,0,0], "bounds_max": [400,5,400], "aoi_cell_size": 16.0, "nav_sample_step": 1.0,
      "grid_width": 400, "grid_height": 400, "walkable_rle": [1, 160000],
      "spawn_points": [{"id":"default","position":[80,0,80],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> d;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &d, &err)) {
        std::printf("FAIL map %s\n", err.c_str());
        return nullptr;
    }
    return d;
}

MapEntity Ent(uint64_t id) {
    MapEntity e;
    e.player_id = id;
    e.player_name = "b";
    e.x = 80.f;
    e.y = 0;
    e.z = 80.f;
    e.hp = 100;
    e.max_hp = 100;
    e.move_speed = 10.f;
    e.gateway_instance_id = "gw";
    e.session_id = "s";
    return e;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::ERROR);
    ::unsetenv("GAMEMESH_AOI_MEMBERSHIP_UNIFIED");
    ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    auto data = MakeMap();
    if (!data)
        return 1;
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(2);
    rt.SetMaxVisible(200);
    MapRuntime::SetNowMsForTest(500000);
    std::string err;
    // 906 人在这一台上光进图就会把 RefreshViewers 跑满数分钟，自动化断言用满员上限 200。
    const int n = 200;
    for (int i = 0; i < n; ++i) {
        AoiPushBatch pushes;
        MapEntity self;
        std::vector<MapEntity> snap;
        MapEntity e = Ent(static_cast<uint64_t>(i + 1));
        e.x = 80.f + static_cast<float>(i % 5) * 0.2f;
        e.z = 80.f + static_cast<float>(i / 5) * 0.05f;
        if (!rt.Enter(1, data, e, &self, &snap, &pushes, &err, 2, 200)) {
            std::printf("FAIL enter %s\n", err.c_str());
            return 1;
        }
    }
    uint64_t seq = 1;
    for (int i = 0; i < n; ++i) {
        AoiPushBatch pushes;
        MapEntity conf;
        const float x = 81.f + static_cast<float>(i % 3) * 0.1f;
        if (rt.Move(1, static_cast<uint64_t>(i + 1), x, 0.f, 82.f, 0.f, seq++, 500000 + i, &conf,
                    &pushes, &err, true) != MapMoveReject::Ok) {
            std::printf("FAIL move setup\n");
            return 1;
        }
    }
    std::atomic<int64_t> move_us{-1};
    std::thread mover([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        AoiPushBatch pushes;
        MapEntity conf;
        const auto t0 = std::chrono::steady_clock::now();
        rt.Move(1, 1, 83.f, 0.f, 82.f, 0.f, seq++, 800000, &conf, &pushes, &err, true);
        move_us.store(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count());
    });
    const auto p0 = std::chrono::steady_clock::now();
    std::vector<AoiPushBatch> batches;
    rt.AdvancePresented(&batches);
    const auto present_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - p0)
                                .count();
    mover.join();
    AoiPushBatch quiet_pushes;
    MapEntity quiet_conf;
    const auto q0 = std::chrono::steady_clock::now();
    const auto quiet_rc = rt.Move(1, 1, 83.2f, 0.f, 82.f, 0.f, seq++, 900000, &quiet_conf,
                                   &quiet_pushes, &err, true);
    const auto quiet_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - q0)
                              .count();
    const auto slice = ServerStats::aoi_present_slice_us_max.load(std::memory_order_relaxed);
    std::printf("present_us=%lld overlap_move_us=%lld quiet_move_us=%lld slice_max_us=%lld entities=%d\n",
                static_cast<long long>(present_us), static_cast<long long>(move_us.load()),
                static_cast<long long>(quiet_us), static_cast<long long>(slice), n);
    rt.ClearForTest();
    if (quiet_rc != MapMoveReject::Ok || quiet_us > 5000 || slice > 5000 || move_us.load() > 5000) {
        std::printf("FAIL move or slice exceeded 5000 us\n");
        return 1;
    }
    std::printf("PASS aoi_present_slice_test\n");
    return 0;
}
