#pragma once

#include "PushReplayCache.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * 跨 Gateway 共享的可靠 Push 回放存储（Redis）。
 * seq 分配与可靠消息写入原子；进程内 PushReplayCache 仅作无 Redis 降级。
 */
class PushReplayStore {
public:
    static PushReplayStore &Instance();

    bool InitFromSessionPrefix(const std::string &key_prefix, size_t cap = 64,
                               int ttl_sec = 7200);
    bool Available() const { return available_; }

    /** 原子分配 server_seq 并追加可靠消息；成功返回 seq>0 */
    uint64_t AppendReliable(uint64_t player_id, const std::string &message_type,
                            const std::string &payload);

    bool ReplayAfter(uint64_t player_id, uint64_t last_acked_seq,
                     std::vector<PushReplayEntry> *out, bool *need_snapshot);

    /** 裁剪 seq<=ack_seq；需调用方已校验 session/fence */
    bool Ack(uint64_t player_id, uint64_t ack_seq);

    uint64_t CurrentSeq(uint64_t player_id);

private:
    PushReplayStore() = default;
    std::string SeqKey(uint64_t player_id) const;
    std::string ListKey(uint64_t player_id) const;

    bool available_ = false;
    std::string key_prefix_ = "gamemesh:dev:";
    size_t cap_ = 64;
    int ttl_sec_ = 7200;
};
