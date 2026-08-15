#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** MapStaticData V1：Unity X/Z 水平面，Y 为高度。walkable_rle = [value, count, ...] */
struct MapVec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

struct MapSpawnPoint {
    std::string id;
    MapVec3 position;
    float yaw = 0;
};

class MapStaticData {
public:
    static bool LoadFromJson(const std::string &json, std::shared_ptr<const MapStaticData> *out,
                             std::string *err);
    /** 读文件并校验 SHA-256（与独立 .sha256 或 expected_hex 比较，小写十六进制） */
    static bool LoadFromFile(const std::string &path, const std::string &expected_sha256_hex,
                             std::shared_ptr<const MapStaticData> *out, std::string *err);

    static std::string NormalizeSha256Hex(std::string hex);
    static bool ReadSha256File(const std::string &path, std::string *hex, std::string *err);

    uint64_t map_template_id() const { return map_template_id_; }
    uint64_t data_version() const { return data_version_; }
    const std::string &scene_name() const { return scene_name_; }
    const std::string &sha256() const { return sha256_; }
    int schema_version() const { return schema_version_; }

    MapVec3 bounds_min() const { return bounds_min_; }
    MapVec3 bounds_max() const { return bounds_max_; }
    float aoi_cell_size() const { return aoi_cell_size_; }
    float nav_sample_step() const { return nav_sample_step_; }
    int grid_width() const { return grid_width_; }
    int grid_height() const { return grid_height_; }
    const MapSpawnPoint &default_spawn() const { return default_spawn_; }
    const std::vector<MapSpawnPoint> &spawns() const { return spawns_; }

    /** col = floor((x-min_x)/step)，row = floor((z-min_z)/step) */
    bool WorldToWalkableCell(float x, float z, int *col, int *row) const;
    bool IsWalkable(float x, float z) const;
    bool CellWalkable(int col, int row) const;
    bool InBounds(float x, float y, float z) const;
    void WorldToAoiCell(float x, float z, int *cell_x, int *cell_z) const;

    /** 共享不可变 grid，实例不得拷贝整表 */
    const std::vector<uint8_t> &walkable() const { return walkable_; }

private:
    MapStaticData() = default;
    static bool Parse(const std::string &json, MapStaticData *out, std::string *err);
    bool ValidateSpawns(std::string *err) const;

    int schema_version_ = 0;
    uint64_t map_template_id_ = 0;
    uint64_t data_version_ = 0;
    std::string scene_name_;
    std::string sha256_;
    MapVec3 bounds_min_;
    MapVec3 bounds_max_;
    float aoi_cell_size_ = 12.f;
    float nav_sample_step_ = 1.f;
    int grid_width_ = 0;
    int grid_height_ = 0;
    std::vector<uint8_t> walkable_;
    std::vector<MapSpawnPoint> spawns_;
    MapSpawnPoint default_spawn_;
};
