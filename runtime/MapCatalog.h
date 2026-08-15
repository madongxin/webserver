#pragma once

#include "MapStaticData.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

/** GameLogic 启动加载的只读地图目录；实例共享同一份 MapStaticData。 */
class MapCatalog {
public:
    static MapCatalog &Instance();

    bool LoadDirectory(const std::string &dir, std::string *err);
    /** 解析 config/maps（或 map_data_dir）；已加载则跳过 */
    bool EnsureDefault(std::string *err);
    void SetMapDataDir(std::string dir);
    const std::string &map_data_dir() const { return map_data_dir_; }

    void SetPublicMapCapacity(uint32_t n);
    uint32_t public_map_capacity() const { return public_capacity_; }
    void SetAoiViewRadiusCells(int n);
    int aoi_view_radius_cells() const { return aoi_view_radius_; }
    void SetMapTickHz(int hz);
    int map_tick_hz() const { return map_tick_hz_; }

    std::shared_ptr<const MapStaticData> Get(uint64_t map_template_id) const;
    bool empty() const;
    void ClearForTest();
    /** 测试注入（不经过目录/哈希文件） */
    void PutForTest(std::shared_ptr<const MapStaticData> data);

private:
    MapCatalog() = default;

    mutable std::mutex mu_;
    std::string map_data_dir_;
    bool loaded_ = false;
    uint32_t public_capacity_ = 50;
    int aoi_view_radius_ = 2;
    int map_tick_hz_ = 10;
    std::unordered_map<uint64_t, std::shared_ptr<const MapStaticData>> maps_;
};
