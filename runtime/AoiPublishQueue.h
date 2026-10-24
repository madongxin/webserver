#pragma once

#include "MapRuntime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

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

struct AoiMoveCandidate {
    uint64_t entity_id = 0;
    int64_t last_sent_ms = 0;
};

/** 一帧里最多保留 max_n 个 MOVE，优先最久没有写出的实体。 */
inline std::vector<uint64_t> PickAoiMoveEntities(std::vector<AoiMoveCandidate> candidates,
                                                 size_t max_n) {
    std::vector<uint64_t> out;
    if (max_n == 0 || candidates.empty())
        return out;
    if (candidates.size() > max_n) {
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(max_n),
                          candidates.end(),
                          [](const AoiMoveCandidate &a, const AoiMoveCandidate &b) {
                              if (a.last_sent_ms != b.last_sent_ms)
                                  return a.last_sent_ms < b.last_sent_ms;
                              return a.entity_id < b.entity_id;
                          });
        candidates.resize(max_n);
    }
    out.reserve(candidates.size());
    for (const auto &c : candidates)
        out.push_back(c.entity_id);
    return out;
}
