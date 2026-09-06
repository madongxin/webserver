#pragma once

#include <cstdint>
#include <string>

enum class SceneKind {
    LegacyPool = 0,
    Line = 1,
    Dungeon = 2,
};

inline SceneKind SceneKindFromString(const std::string &s) {
    if (s == "LINE")
        return SceneKind::Line;
    if (s == "DUNGEON")
        return SceneKind::Dungeon;
    return SceneKind::LegacyPool;
}

inline const char *SceneKindToString(SceneKind k) {
    switch (k) {
    case SceneKind::Line:
        return "LINE";
    case SceneKind::Dungeon:
        return "DUNGEON";
    case SceneKind::LegacyPool:
    default:
        return "LEGACY_POOL";
    }
}

inline bool IsLineKind(const std::string &s) {
    return SceneKindFromString(s) == SceneKind::Line;
}

inline bool IsDungeonKind(const std::string &s) {
    return SceneKindFromString(s) == SceneKind::Dungeon;
}

struct MapScenePolicy {
    SceneKind kind = SceneKind::LegacyPool;
    uint32_t soft_cap = 200;
    uint32_t hard_cap = 400;
    uint32_t max_lines = 8;
    uint32_t min_lines = 1;
    uint32_t empty_close_delay = 0;  // 0=LINE 300 / DUNGEON 30
    int aoi_view_radius_cells = -1;  // <0 用进程默认
    float spawn_scatter_radius = 0.f;
};
