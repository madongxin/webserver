/**
 * S2：Map JSON / SHA-256 / RLE [value,count] / XZ cell 数学 / 出生点。
 */
#include "FileHash.h"
#include "GameMeshPaths.h"
#include "MapCatalog.h"
#include "MapStaticData.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

std::string MapsDir() {
    std::string d;
    if (GameMeshPaths::ResolveProjectSubdir("config/maps", &d))
        return d;
    return "config/maps";
}

}  // namespace

int main() {
    std::string err;
    std::shared_ptr<const MapStaticData> data;

    const std::string tiny = R"JSON({
      "schema_version": 1,
      "map_template_id": 9001,
      "scene_name": "Tiny",
      "data_version": 1,
      "bounds_min": [0, 0, 0],
      "bounds_max": [4, 2, 4],
      "aoi_cell_size": 2.0,
      "nav_sample_step": 1.0,
      "grid_width": 4,
      "grid_height": 4,
      "walkable_rle": [1, 10, 0, 2, 1, 4],
      "spawn_points": [{"id":"default","position":[0.5, 0.1, 0.5],"yaw":0}]
    })JSON";
    Expect(MapStaticData::LoadFromJson(tiny, &data, &err), "tiny json load");
    if (!data)
        return 1;
    Expect(data->walkable().size() == 16, "rle 4x4=16");
    Expect(data->CellWalkable(0, 0), "first cells walkable");
    Expect(!data->CellWalkable(2, 2), "rle hole unwalkable");  // 10 walkable then 2 blocked
    Expect(data->IsWalkable(0.5f, 0.5f), "spawn walkable");
    int col = -1, row = -1;
    Expect(data->WorldToWalkableCell(0.0f, 0.0f, &col, &row) && col == 0 && row == 0, "cell 0,0");
    Expect(data->WorldToWalkableCell(3.9f, 0.1f, &col, &row) && col == 3 && row == 0, "cell 3,0");
    int ax = 0, az = 0;
    data->WorldToAoiCell(0.5f, 0.5f, &ax, &az);
    Expect(ax == 0 && az == 0, "aoi cell 0");
    data->WorldToAoiCell(2.1f, 2.1f, &ax, &az);
    Expect(ax == 1 && az == 1, "aoi cell 1 (size 2)");

    std::shared_ptr<const MapStaticData> bad;
    Expect(!MapStaticData::LoadFromJson(
               R"JSON({"schema_version":1,"map_template_id":1,"data_version":1,
                 "bounds_min":[0,0,0],"bounds_max":[2,1,2],"aoi_cell_size":1,
                 "nav_sample_step":1,"grid_width":2,"grid_height":2,
                 "walkable_rle":[1,3],"spawn_points":[{"id":"d","position":[0.5,0,0.5],"yaw":0}]})JSON",
               &bad, &err),
           "odd rle / count mismatch rejected");
    Expect(!MapStaticData::LoadFromJson(
               R"JSON({"schema_version":1,"map_template_id":1,"data_version":1,
                 "bounds_min":[0,0,0],"bounds_max":[2,1,2],"aoi_cell_size":1,
                 "nav_sample_step":1,"grid_width":2,"grid_height":2,
                 "walkable_rle":[0,4],"spawn_points":[{"id":"d","position":[0.5,0,0.5],"yaw":0}]})JSON",
               &bad, &err),
           "unwalkable spawn rejected");

    const std::string dir = MapsDir();
    std::string expect_hash;
    Expect(MapStaticData::ReadSha256File(dir + "/map_1001.json.sha256", &expect_hash, &err),
           "read map_1001.sha256");
    const std::string file_hash = FileHasher::HashFile(dir + "/map_1001.json");
    Expect(file_hash == expect_hash, "file hash matches sidecar");
    Expect(!MapStaticData::LoadFromFile(dir + "/map_1001.json", std::string(64, '0'), &bad, &err),
           "wrong hash rejected");
    Expect(MapStaticData::LoadFromFile(dir + "/map_1001.json", expect_hash, &data, &err),
           "load map_1001");
    if (data) {
        Expect(data->map_template_id() == 1001, "template 1001");
        Expect(data->grid_width() == 171 && data->grid_height() == 162, "171x162");
        Expect(data->walkable().size() == 171u * 162u, "rle expands to grid");
        Expect(data->aoi_cell_size() == 12.f, "1001 aoi 12");
        Expect(data->nav_sample_step() == 1.f, "1001 step 1");
        Expect(data->IsWalkable(-28.5f, -7.25f), "luna spawn walkable");
        Expect(data->WorldToWalkableCell(-28.5f, -7.25f, &col, &row) && col == 65 && row == 34,
               "spawn cell 65,34");
    }

    std::string hash_1002;
    Expect(MapStaticData::ReadSha256File(dir + "/1002.grid.json.sha256", &hash_1002, &err),
           "read 1002.sha256");
    Expect(hash_1002 == "46d5bb506de2f0418a85fce8d8e285dfec664023122b2692cecf818a26be75d9",
           "1002 sidecar hash");
    Expect(FileHasher::HashFile(dir + "/1002.grid.json") == hash_1002, "1002 file hash matches sidecar");
    std::shared_ptr<const MapStaticData> data1002;
    Expect(MapStaticData::LoadFromFile(dir + "/1002.grid.json", hash_1002, &data1002, &err),
           "load 1002");
    if (data1002) {
        Expect(data1002->map_template_id() == 1002, "template 1002");
        Expect(data1002->data_version() == 1, "1002 data_version 1");
        Expect(data1002->scene_name() == "TerrainDemoScene", "1002 scene");
        Expect(data1002->grid_width() == 498 && data1002->grid_height() == 498, "498x498");
        Expect(data1002->aoi_cell_size() == 32.f, "1002 aoi 32");
        Expect(data1002->nav_sample_step() == 8.f, "1002 step 8");
        Expect(data1002->default_spawn().id == "default", "1002 spawn id");
        Expect(data1002->IsWalkable(179.3f, 324.2f), "1002 spawn walkable");
        const MapVec3 &sp = data1002->default_spawn().position;
        Expect(std::fabs(sp.x - 179.3f) < 0.01f && std::fabs(sp.y + 18.687f) < 0.01f &&
                   std::fabs(sp.z - 324.2f) < 0.01f,
               "1002 spawn xyz");
        Expect(std::fabs(data1002->default_spawn().yaw - 180.f) < 0.01f, "1002 spawn yaw 180");
        Expect(data1002->sha256() == hash_1002, "1002 loaded sha256");
    }

    MapCatalog::Instance().ClearForTest();
    Expect(MapCatalog::Instance().LoadDirectory(dir, &err), "catalog load");
    Expect(MapCatalog::Instance().Get(1001) != nullptr, "catalog has 1001");
    Expect(MapCatalog::Instance().Get(1001)->sha256() == expect_hash, "catalog hash");
    Expect(MapCatalog::Instance().Get(1001)->aoi_cell_size() == 12.f, "catalog 1001 aoi");
    Expect(MapCatalog::Instance().Get(1002) != nullptr, "catalog has 1002");
    Expect(MapCatalog::Instance().Get(1002)->sha256() == hash_1002, "catalog 1002 hash");
    Expect(MapCatalog::Instance().Get(1002)->aoi_cell_size() == 32.f, "catalog 1002 aoi");
    Expect(MapCatalog::Instance().Get(1002)->nav_sample_step() == 8.f, "catalog 1002 step");
    Expect(MapCatalog::Instance().ManifestEntries().size() == 4, "manifest 4 maps");
    {
        MapScenePolicy pol;
        Expect(MapCatalog::Instance().GetScenePolicy(1001, &pol) &&
                   pol.kind == SceneKind::LegacyPool,
               "1001 legacy kind");
        Expect(MapCatalog::Instance().GetScenePolicy(1002, &pol) &&
                   pol.kind == SceneKind::Line && pol.soft_cap == 200 &&
                   pol.hard_cap == 400 && pol.aoi_view_radius_cells == 1 &&
                   pol.spawn_scatter_radius == 160.f,
               "1002 LINE 200/400");
        Expect(MapCatalog::Instance().GetScenePolicy(1001, &pol) &&
                   pol.spawn_scatter_radius == 4.f,
               "1001 scatter 4");
        Expect(MapCatalog::Instance().Get(1101) != nullptr, "catalog has 1101");
        Expect(MapCatalog::Instance().GetScenePolicy(1101, &pol) && pol.kind == SceneKind::Line &&
                   pol.soft_cap == 2 && pol.hard_cap == 3 && pol.max_lines == 4 &&
                   pol.min_lines == 1 && pol.aoi_view_radius_cells == 1 &&
                   pol.spawn_scatter_radius == 3.f,
               "1101 LINE policy");
        Expect(MapCatalog::Instance().Get(2101) != nullptr, "catalog has 2101");
        Expect(MapCatalog::Instance().GetScenePolicy(2101, &pol) && pol.kind == SceneKind::Dungeon &&
                   pol.hard_cap == 5 && pol.empty_close_delay == 30,
               "2101 DUNGEON policy");
    }
    {
        float x = 0, y = 0, z = 0;
        Expect(data->FindScatteredSpawn(-28.5f, 0.f, -7.25f, 4.f, 42, &x, &y, &z),
               "scatter walkable");
        Expect(data->IsWalkable(x, z), "scattered cell walkable");
    }
    Expect(MapCatalog::Instance().gameplay_config_version() == 1, "gameplay_config_version");
    Expect(MapCatalog::Instance().map_manifest_version() == 1, "map_manifest_version");
    Expect(!MapCatalog::Instance().ManifestEntries().empty(), "manifest entries");

    if (fails) {
        std::printf("map_static_data_test FAIL count=%d last=%s\n", fails, err.c_str());
        return 1;
    }
    std::printf("OK map_static_data_test\n");
    return 0;
}
