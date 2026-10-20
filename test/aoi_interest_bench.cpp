/**
 * AOI interest microbench. Prints timings for this machine only.
 * Does not claim a multi-core or 10k-CCU result.
 */
#include "MapRuntime.h"
#include "MapStaticData.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

std::shared_ptr<const MapStaticData> MakeOpen() {
    const std::string json = R"JSON({
      "schema_version": 1,
      "map_template_id": 9101,
      "scene_name": "Open",
      "data_version": 1,
      "bounds_min": [0, 0, 0],
      "bounds_max": [400, 5, 400],
      "aoi_cell_size": 32.0,
      "nav_sample_step": 1.0,
      "grid_width": 400,
      "grid_height": 400,
      "walkable_rle": [1, 160000],
      "spawn_points": [{"id":"default","position":[1, 0, 1],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> d;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &d, &err)) {
        std::printf("FAIL make open: %s\n", err.c_str());
        return nullptr;
    }
    return d;
}

MapEntity Ent(uint64_t id, float x, float z) {
    MapEntity e;
    e.player_id = id;
    e.x = x;
    e.y = 0;
    e.z = z;
    e.move_speed = 50.f;
    e.gateway_instance_id = "gw-0";
    e.session_id = "s" + std::to_string(id);
    return e;
}

void Bench(const std::shared_ptr<const MapStaticData> &data, int instances, int per_instance) {
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    for (int inst = 1; inst <= instances; ++inst) {
        for (int i = 1; i <= per_instance; ++i) {
            const uint64_t id = static_cast<uint64_t>(inst) * 100000 + static_cast<uint64_t>(i);
            const float x = static_cast<float>((i % 20) * 3);
            const float z = static_cast<float>((i / 20) * 3);
            if (!rt.Enter(static_cast<uint64_t>(inst), data, Ent(id, x, z), &self, &snap, &pushes,
                          &err)) {
                std::printf("enter failed inst=%d id=%llu\n", inst, static_cast<unsigned long long>(id));
                return;
            }
        }
    }
    const int moves = 200;
    MapEntity conf;
    const auto t0 = std::chrono::steady_clock::now();
    int events = 0;
    for (int n = 0; n < moves; ++n) {
        pushes = {};
        const uint64_t id = 100000 + 1;
        const float x = 1.f + static_cast<float>(n % 10);
        rt.Move(1, id, x, 0, 1, 0, static_cast<uint64_t>(n + 1), 1000 + n * 100, &conf, &pushes,
                &err, true);
        events += static_cast<int>(pushes.events.size());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("instances=%d per_instance=%d moves=%d events=%d ms=%.2f\n", instances,
                per_instance, moves, events, ms);
}

}  // namespace

int main() {
    auto data = MakeOpen();
    if (!data)
        return 1;
    std::printf("max_visible=%d\n", MapRuntime::Instance().max_visible());
    const int populations[] = {100, 200, 400, 1000};
    for (int n : populations)
        Bench(data, 1, n);
    Bench(data, 4, 100);
    std::printf("OK aoi_interest_bench\n");
    return 0;
}
