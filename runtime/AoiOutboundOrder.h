#pragma once

#include "AoiMoveCoalesceLedger.h"
#include "MoveSendConfig.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

/**
 * 合并槽和急送谁先上线。网关与测试共用，避免两套顺序。
 *
 * AoiStripEntityIds 只从帧里取出非 MOVE 的实体，给连接上还没写出第一个字节的那一帧用。
 * 它自己不清 MoveCoalesceSlot，也不清账本。清槽、墓碑和 resync 过滤走下面这些函数。
 */
struct AoiOutboundBooks {
    std::unordered_set<uint64_t> notified;
    std::unordered_map<uint64_t, int64_t> tomb_until_ms;
};

/** LEAVE 之后这段时间拒收该实体的 MOVE。收到 ENTER 提前解除。待压测校准。 */
inline int AoiMoveTombstoneMs() { return MoveEnvInt("GAMEMESH_AOI_MOVE_TOMB_MS", 5000, 0, 120000); }

inline bool KeepCoalescedMoveSeq(bool has_kept, uint64_t kept_seq, uint64_t incoming_seq) {
    return !has_kept || incoming_seq >= kept_seq;
}

/** ENTER/LEAVE 要从合并槽、账本和 resync 里去掉该实体。 */
inline bool UrgentDropsCoalescedMove(int op) { return op == 1 || op == 3; }

inline bool TombstoneBlocksMove(const AoiOutboundBooks &books, uint64_t id, int64_t now_ms) {
    if (id == 0)
        return false;
    auto it = books.tomb_until_ms.find(id);
    if (it == books.tomb_until_ms.end())
        return false;
    return now_ms < it->second;
}

inline void NoteEnterVisible(AoiOutboundBooks *books, uint64_t id) {
    if (!books || id == 0)
        return;
    books->tomb_until_ms.erase(id);
    books->notified.insert(id);
}

inline void NoteLeaveVisible(AoiOutboundBooks *books, uint64_t id, int64_t now_ms) {
    if (!books || id == 0)
        return;
    books->notified.erase(id);
    const int hold = AoiMoveTombstoneMs();
    if (hold <= 0) {
        books->tomb_until_ms.erase(id);
        return;
    }
    books->tomb_until_ms[id] = now_ms + static_cast<int64_t>(hold);
}

/** 清槽后的旧 MOVE。可见集合开关打开时整批丢掉，否则还要看这个人是否仍被通知。 */
inline bool ReplayClearedMoves(bool visible_set_on) { return !visible_set_on; }

inline bool ShouldReplayClearedMove(bool visible_set_on, const AoiOutboundBooks &books, uint64_t id,
                                     int64_t now_ms) {
    if (!ReplayClearedMoves(visible_set_on) || id == 0)
        return false;
    if (books.notified.count(id) == 0)
        return false;
    return !TombstoneBlocksMove(books, id, now_ms);
}

template <typename SlotMap, typename EraseResync>
void ApplyUrgentAoi(SlotMap *by_entity, AoiMoveCoalesceLedger *ledger, EraseResync erase_resync,
                    AoiOutboundBooks *books, int op, uint64_t entity_id, int64_t now_ms) {
    if (entity_id == 0 || op == 2 || !UrgentDropsCoalescedMove(op))
        return;
    if (by_entity)
        by_entity->erase(entity_id);
    if (ledger)
        ledger->Forget(entity_id);
    erase_resync(entity_id);
    if (op == 1)
        NoteEnterVisible(books, entity_id);
    else if (op == 3)
        NoteLeaveVisible(books, entity_id, now_ms);
}

template <typename Map>
void ApplyUrgentToCoalesceSlot(Map *by_entity, int op, uint64_t entity_id) {
    ApplyUrgentAoi(by_entity, static_cast<AoiMoveCoalesceLedger *>(nullptr), [](uint64_t) {},
                   static_cast<AoiOutboundBooks *>(nullptr), op, entity_id, 0);
}
