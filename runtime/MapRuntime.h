#pragma once

#include "MapStaticData.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class MapMoveReject {
    Ok = 0,
    NotOnMap,
    NotFinite,
    OutOfBounds,
    Unwalkable,
    TooFast,
    StaleSeq,
    Disconnected,
};

struct MapEntity {
    uint64_t player_id = 0;
    std::string player_name;
    float x = 0;
    float y = 0;
    float z = 0;
    float yaw = 0;
    int32_t hp = 0;
    int32_t max_hp = 0;
    uint64_t state_seq = 0;
    bool connected = true;
    std::string gateway_instance_id;
    std::string session_id;
    uint64_t last_client_seq = 0;
    int64_t last_move_server_ms = 0;
    float move_speed = 10.f;
};

struct AoiRecipientEvent {
    uint64_t recipient_id = 0;
    std::string gateway_instance_id;
    std::string session_id;
    int op = 0;  // 1=ENTER 2=MOVE 3=LEAVE
    MapEntity snapshot;
};

struct AoiPushBatch {
    uint64_t map_instance_id = 0;
    std::vector<AoiRecipientEvent> events;
};

class MapRuntime {
public:
    static MapRuntime &Instance();

    void SetViewRadiusCells(int n);
    int view_radius_cells() const { return view_radius_; }

    bool Enter(uint64_t map_instance_id, std::shared_ptr<const MapStaticData> data,
               MapEntity entity, MapEntity *self_out, std::vector<MapEntity> *aoi_snapshot,
               AoiPushBatch *pushes, std::string *err);
    MapMoveReject Move(uint64_t map_instance_id, uint64_t player_id, float x, float y, float z,
                       float yaw, uint64_t client_seq, int64_t now_ms, MapEntity *confirmed,
                       AoiPushBatch *pushes, std::string *err_code);
    bool Leave(uint64_t map_instance_id, uint64_t player_id, AoiPushBatch *pushes);
    void LeaveAll(uint64_t player_id, AoiPushBatch *pushes);
    bool Disconnect(uint64_t player_id, AoiPushBatch *pushes);
    bool Reconnect(uint64_t player_id, const std::string &gateway_instance_id,
                   const std::string &session_id, MapEntity *self_out,
                   std::vector<MapEntity> *aoi_snapshot, AoiPushBatch *pushes);

    bool HasPlayer(uint64_t map_instance_id, uint64_t player_id) const;
    uint32_t ConnectedCount(uint64_t map_instance_id) const;
    uint64_t PlayerMap(uint64_t player_id) const;
    void ClearForTest();

private:
    MapRuntime() = default;

    struct InstanceState {
        std::shared_ptr<const MapStaticData> data;
        std::unordered_map<uint64_t, MapEntity> entities;
        std::unordered_map<uint64_t, std::unordered_set<uint64_t>> cells;
    };

    static uint64_t CellKey(int cx, int cz);
    static int64_t NowMs();
    std::unordered_set<uint64_t> VisibleIds(const InstanceState &st, const MapEntity &e) const;
    void PlaceInCell(InstanceState *st, const MapEntity &e);
    void RemoveFromCell(InstanceState *st, const MapEntity &e);
    void EmitTo(AoiPushBatch *pushes, uint64_t map_id, const MapEntity &recipient, int op,
                const MapEntity &snapshot) const;

    mutable std::mutex mu_;
    int view_radius_ = 2;
    std::unordered_map<uint64_t, std::unique_ptr<InstanceState>> maps_;
    std::unordered_map<uint64_t, uint64_t> player_map_;
};
