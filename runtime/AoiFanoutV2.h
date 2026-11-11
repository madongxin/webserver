#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

/** 可见集合选择。距离近的在前；id 小的优先。不依赖 socket。 */
struct AoiCand {
    uint64_t id = 0;
    float d2 = 0;
    bool in_enter = false;
    bool in_leave = false;
    bool in_notified = false;
};

inline std::vector<uint64_t> SelectVisible(std::vector<AoiCand> cands, int cap, int slack) {
    if (cap < 1)
        cap = 1;
    if (slack < 0)
        slack = 0;
    if (slack >= cap)
        slack = cap - 1;
    std::sort(cands.begin(), cands.end(), [](const AoiCand &a, const AoiCand &b) {
        if (a.d2 < b.d2)
            return true;
        if (b.d2 < a.d2)
            return false;
        return a.id < b.id;
    });
    std::vector<uint64_t> desired;
    std::vector<char> kept(cands.size(), 0);
    for (size_t i = 0; i < cands.size(); ++i) {
        const auto &c = cands[i];
        if (!c.in_notified || !c.in_leave)
            continue;
        const int rank = static_cast<int>(i) + 1;
        if (rank <= cap + slack) {
            kept[i] = 1;
            desired.push_back(c.id);
        }
    }
    if (static_cast<int>(desired.size()) > cap)
        desired.resize(static_cast<size_t>(cap));
    int held = static_cast<int>(desired.size());
    // 集合不足 N-slack 时按名次补到 N。已经站在这条线以上时，新来的人必须排进 N-slack。
    const bool underfilled = held < cap - slack;
    for (size_t i = 0; i < cands.size() && held < cap; ++i) {
        if (kept[i])
            continue;
        const auto &c = cands[i];
        if (!c.in_enter)
            continue;
        const int rank = static_cast<int>(i) + 1;
        if (!underfilled && rank > cap - slack)
            continue;
        desired.push_back(c.id);
        ++held;
    }
    return desired;
}
