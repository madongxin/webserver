#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

/**
 * 网关已经找不到会话时，短时间内不再给这个收件人组 AOI。
 * BindPlayer 成功后清除。不删除地图实体，也不等于 Logout。
 */
class AoiRecipientGate {
public:
    static AoiRecipientGate &Instance();

    bool Allowed(uint64_t player_id) const;
    void Suppress(uint64_t player_id);
    void Clear(uint64_t player_id);
    void ClearAll();

private:
    AoiRecipientGate() = default;
    mutable std::mutex mu_;
    std::unordered_set<uint64_t> suppressed_;
};
