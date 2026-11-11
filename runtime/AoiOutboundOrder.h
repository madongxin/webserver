#pragma once

#include <cstdint>
#include <unordered_map>

/**
 * 合并槽和急送谁先上线。网关与测试共用，避免两套顺序。
 *
 * AoiStripEntityIds / SendBufV2::PushControl 只丢掉连接上还没写出第一个字节的那一帧位置，
 * 不清 MoveCoalesceSlot::by_entity，也不清 AoiMoveCoalesceLedger 里的条目。
 */
inline bool KeepCoalescedMoveSeq(bool has_kept, uint64_t kept_seq, uint64_t incoming_seq) {
    return !has_kept || incoming_seq >= kept_seq;
}

/** 当前网关：ENTER/LEAVE 急送不从合并槽删掉该实体尚未写出的 MOVE。 */
inline bool UrgentDropsCoalescedMove(int op) {
    (void)op;
    return false;
}

/** 清槽后的旧 MOVE。可见集合开关打开时丢掉，否则整批 Offer 回槽。 */
inline bool ReplayClearedMoves(bool visible_set_on) { return !visible_set_on; }

template <typename Map>
void ApplyUrgentToCoalesceSlot(Map *by_entity, int op, uint64_t entity_id) {
    if (!by_entity || entity_id == 0 || op == 2)
        return;
    if (UrgentDropsCoalescedMove(op))
        by_entity->erase(entity_id);
}
