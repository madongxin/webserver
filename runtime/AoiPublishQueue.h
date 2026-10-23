#pragma once

#include "MapRuntime.h"

#include <cstddef>
#include <deque>

/** 队列里只含 MOVE（op=2）的批次可以丢掉。ENTER/LEAVE 必须留下。 */
inline bool AoiBatchIsMoveOnly(const AoiPushBatch &batch) {
    for (const auto &ev : batch.events) {
        if (ev.op != 2)
            return false;
    }
    return true;
}

/**
 * 批次超过 cap 时，从最旧的一批开始丢掉纯 MOVE 批次。
 * 队列里只剩带 ENTER/LEAVE 的批次时停止，允许暂时超过 cap。
 * 返回丢掉的事件数。
 */
inline size_t ShedAoiMoveBatches(std::deque<AoiPushBatch> *q, size_t cap) {
    if (!q)
        return 0;
    size_t shed = 0;
    while (q->size() > cap) {
        auto it = q->begin();
        for (; it != q->end(); ++it) {
            if (AoiBatchIsMoveOnly(*it))
                break;
        }
        if (it == q->end())
            break;
        shed += it->events.size();
        q->erase(it);
    }
    return shed;
}
