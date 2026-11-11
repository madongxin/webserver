#pragma once

#include "MapStaticData.h"

#include <atomic>
#include <chrono>
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
    int64_t last_displace_ms = 0;
    int64_t disconnected_ms = 0;
    int64_t last_client_time_ms = 0;
    float move_speed = 10.f;
    float vx = 0;
    float vz = 0;
    int64_t move_start_ms = 0;
    // 别人看见的位置。大步移动时权威点先到终点，呈现点沿速度走过去，避免一帧离开视野。
    float aoi_x = 0;
    float aoi_z = 0;
    bool aoi_live = false;
    float PresentedX() const { return aoi_live ? aoi_x : x; }
    float PresentedZ() const { return aoi_live ? aoi_z : z; }
};

struct AoiRecipientEvent {
    uint64_t recipient_id = 0;
    std::string gateway_instance_id;
    std::string session_id;
    int op = 0;  // 1=ENTER 2=MOVE 3=LEAVE
    MapEntity snapshot;
    uint64_t notify_version = 0;
};

struct AoiPushBatch {
    uint64_t map_instance_id = 0;
    int64_t enqueued_ms = 0;
    std::vector<AoiRecipientEvent> events;
};

class MapRuntime {
public:
    static MapRuntime &Instance();

    void SetViewRadiusCells(int n);
    int view_radius_cells() const { return view_radius_; }
    // 新实例冻结的可见人数。已创建的实例不随后续调用改变。范围 1..200。
    void SetMaxVisible(int n);
    int max_visible() const { return max_visible_; }

    /** EnterMap / 重连之后把移动序号窗口清零，下一个合法 seq 从 1 起。 */
    void ResetClientSeq(uint64_t player_id);
    bool Enter(uint64_t map_instance_id, std::shared_ptr<const MapStaticData> data,
               MapEntity entity, MapEntity *self_out, std::vector<MapEntity> *aoi_snapshot,
               AoiPushBatch *pushes, std::string *err, int view_radius_cells = -1,
               int max_visible = -1);
    /** 关闭已回收实例（空线/空本 CLOSED） */
    bool Unload(uint64_t map_instance_id);
    MapMoveReject Move(uint64_t map_instance_id, uint64_t player_id, float x, float y, float z,
                       float yaw, uint64_t client_seq, int64_t now_ms, MapEntity *confirmed,
                       AoiPushBatch *pushes, std::string *err_code, bool ignore_speed = false,
                       int64_t client_time_ms = 0);
    bool Leave(uint64_t map_instance_id, uint64_t player_id, AoiPushBatch *pushes);
    void LeaveAll(uint64_t player_id, AoiPushBatch *pushes);
    bool Disconnect(uint64_t player_id, AoiPushBatch *pushes);
    /** 宽限到期后删掉断线实体，并再给周围还在线的人发一次 LEAVE。 */
    size_t ExpireDisconnected(int64_t grace_ms, AoiPushBatch *pushes);
    bool Reconnect(uint64_t player_id, const std::string &gateway_instance_id,
                   const std::string &session_id, MapEntity *self_out,
                   std::vector<MapEntity> *aoi_snapshot, AoiPushBatch *pushes);

    /**
     * 删掉不在 keep 里的实体并发 LEAVE。
     * 宽限到期只清 Redis occ、不 Leave 时，内存里的 bot 会进下一次 EnterMap 快照。
     */
    size_t DropExcept(uint64_t map_instance_id, const std::unordered_set<uint64_t> &keep,
                      AoiPushBatch *pushes);
    bool HasPlayer(uint64_t map_instance_id, uint64_t player_id) const;
    uint32_t ConnectedCount(uint64_t map_instance_id) const;
    /** 本实例里除 except_player 以外的人的 XZ。人是一个一个进的，用来躲开已经落下去的点。 */
    void CollectXz(uint64_t map_instance_id, uint64_t except_player,
                   std::vector<std::pair<float, float>> *out) const;
    uint64_t PlayerMap(uint64_t player_id) const;
    bool SnapshotView(uint64_t player_id, MapEntity *self_out, std::vector<MapEntity> *aoi_snapshot,
                      uint64_t *map_instance_id) const;

    /**
     * 全量快照用的兴趣集合，与 AOI 推送的 VisibleIds 同一套规则。
     * 不读 visible_targets 缓存（那份可能还没写上人），也不用分线人数。
     * ready=false 时调用方必须回可重试错误，不能把 interest 空列表当成“周围没人”。
     */
    struct FullView {
        bool ready = false;
        MapEntity self;
        std::vector<MapEntity> interest;
        uint64_t map_instance_id = 0;
        float radius_m = 32.f;
        float cell_m = 0.f;
        int radius_cells = 0;
        size_t cache_n = 0;
        const char *step = "not_on_map";
    };
    bool CollectFullView(uint64_t player_id, FullView *out) const;
    std::vector<uint64_t> ListPlayers() const;
    int TotalEntityCount() const;
    void ClearForTest();
    /** 测试里推进服务器时钟。0 表示用真实单调钟。 */
    static void SetNowMsForTest(int64_t ms);
    /** 最近 5 秒内坐标变过的算移动，其余在线实体算静止。没有人时两个输出都是 0。 */
    void CopyMotionCounts(int64_t *moving, int64_t *stationary) const;
    /** 把大步移动的呈现点朝权威坐标推进 0.2 秒，并补上因此产生的进出视野。 */
    void AdvancePresented(std::vector<AoiPushBatch> *batches);
    /**
     * 网关接受 ENTER/LEAVE 之后回调。
     * ok 表示这一帧进了网关的应用发送缓冲，或当场 write 返回成功。
     * 它不是内核写完，也不是客户端 ACK。逻辑进程没有每个 AOI 帧的内核完成回调，所以没有改成内核确认。
     * 200ms 入队超时后回调仍可能写出，同一版本的 LEAVE 会再来一次；客户端应忽略删除一个已经不存在的实体。
     * 更新的 ENTER 记下更大的版本后，更老的 LEAVE 在交给网关之前丢掉。
     */
    void NoteAoiWrite(uint64_t viewer_id, uint64_t subject_id, int op, bool ok, uint64_t version);
    int LeavePendingCountForTest(uint64_t viewer_id) const;

private:
    MapRuntime() = default;

    struct InstanceState {
        mutable std::mutex mu;
        std::shared_ptr<const MapStaticData> data;
        std::unordered_map<uint64_t, MapEntity> entities;
        std::unordered_map<uint64_t, std::unordered_set<uint64_t>> cells;
        // viewer -> targets this viewer currently subscribes to (cap applied here).
        std::unordered_map<uint64_t, std::unordered_set<uint64_t>> visible_targets;
        // target -> viewers who subscribed to that target. MOVE uses this, not the mover's own set.
        std::unordered_map<uint64_t, std::unordered_set<uint64_t>> watchers;
        // 离开后 LEAVE 还没被网关确认。按 1s、2s、4s、最大 5s 退避，直到确认、断线、回到视野或超过总时长。
        struct LeaveAgain {
            int64_t first_ms = 0;
            int64_t last_ms = 0;
            uint64_t version = 0;
            int tries = 0;
            MapEntity snap;
        };
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, LeaveAgain>> leave_again;
        // subject -> viewer -> 已经交给网关的最新 ENTER/LEAVE 版本。更老的 LEAVE 不再发。
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> interest_version;
        uint64_t next_leave_version = 1;
        enum class AoiPending : uint8_t { None = 0, Enter = 1, Leave = 2 };
        struct NotifiedItem {
            uint64_t version = 0;
            AoiPending pending = AoiPending::None;
            int64_t since_ms = 0;
        };
        // 观察者 -> 实体。pending=Leave 表示客户端还应删掉，网关还没写入。
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, NotifiedItem>> notified;
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, int64_t>> last_left_ms;
        struct MoveNotify {
            int64_t ms = 0;
            float x = 0;
            float z = 0;
            float vx = 0;
            float vz = 0;
            int64_t move_start_ms = 0;
            bool has = false;
        };
        // watcher -> target -> 上次发出的 MOVE。外推关掉时只用 ms、x、z。
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, MoveNotify>> move_notify_ms;
        // 该实体已经发出的 AOI 事件时间。下一条不能更早，同一条 state_seq 沿途也要往前走。
        std::unordered_map<uint64_t, int64_t> last_aoi_event_ms;
        int view_radius = -1;
        int max_visible = 200;  // frozen when the instance is created
        uint64_t epoch = 0;
        std::atomic<bool> unloaded{false};
        std::atomic<int> entity_count{0};
    };

    static uint64_t CellKey(int cx, int cz);
    static int64_t NowMs();
    std::unordered_set<uint64_t> VisibleIds(const InstanceState &st, const MapEntity &e) const;
    std::vector<uint64_t> NeighborhoodIds(const InstanceState &st, float x, float z) const;
    void ForgetViewer(InstanceState *st, uint64_t viewer_id);
    // silent_viewer updates indexes but does not receive ENTER/LEAVE (enter snapshot covers them).
    // mover_id != 0 emits MOVE to viewers who already subscribed and still subscribe.
    void RefreshViewers(InstanceState *st, const std::vector<uint64_t> &viewer_ids, uint64_t mover_id,
                        uint64_t silent_viewer, uint64_t map_instance_id, AoiPushBatch *pushes);
    void RemovePlayerInterest(InstanceState *st, uint64_t map_instance_id, uint64_t player_id,
                              AoiPushBatch *pushes, bool erase_entity);
    void FillSnapshot(const InstanceState &st, uint64_t viewer_id, std::vector<MapEntity> *aoi_snapshot) const;
    void PlaceInCell(InstanceState *st, const MapEntity &e);
    void RemoveFromCell(InstanceState *st, const MapEntity &e);
    void EmitTo(InstanceState *st, AoiPushBatch *pushes, uint64_t map_id, const MapEntity &recipient,
                int op, const MapEntity &snapshot);
    void RememberLeave(InstanceState *st, AoiPushBatch *batch, const MapEntity &subject, int64_t now_ms);
    void RetryPendingLeaves(InstanceState *st, uint64_t map_id, int64_t now_ms, AoiPushBatch *batch);
    void SyncVisibleSubject(InstanceState *st, MapEntity &subject, int64_t now_ms, uint64_t map_id,
                            AoiPushBatch *batch);
    std::vector<uint64_t> NeighborhoodIdsRadius(const InstanceState &st, float x, float z, int radius) const;
    bool InCellRadius(const InstanceState &st, float x0, float z0, float x1, float z1, int radius) const;
    bool AllowMoveAoi(InstanceState *st, const MapEntity &watcher, const MapEntity &subject,
                      int64_t now_ms) const;

    int ResolvedRadius(const InstanceState &st) const;
    /** 锁等待记在构造前，持锁时间记在析构。调用方只把它当作用域锁。 */
    class InstLock {
    public:
        InstLock() = default;
        explicit InstLock(std::unique_lock<std::mutex> lk);
        InstLock(InstLock &&o) noexcept;
        InstLock &operator=(InstLock &&o) noexcept;
        bool owns_lock() const { return live_; }
        void unlock() { Release(); }
        ~InstLock();
        InstLock(const InstLock &) = delete;
        InstLock &operator=(const InstLock &) = delete;

    private:
        void Release() noexcept;
        std::unique_lock<std::mutex> lk_;
        std::chrono::steady_clock::time_point held_at_{};
        bool live_ = false;
    };
    InstLock LockInst(const std::shared_ptr<InstanceState> &st) const;
    // Registry only. Does not take the instance lock.
    bool PublishPlayer(uint64_t player_id, uint64_t map_instance_id,
                       const std::shared_ptr<InstanceState> &st);
    void RetireIfEmpty(uint64_t map_instance_id, const std::shared_ptr<InstanceState> &st);

    mutable std::mutex mu_;  // maps_ + player_map_ only; not held across AOI scans
    int view_radius_ = 2;
    int max_visible_ = 200;
    uint64_t next_epoch_ = 1;
    std::unordered_map<uint64_t, std::shared_ptr<InstanceState>> maps_;
    std::unordered_map<uint64_t, uint64_t> player_map_;
};
