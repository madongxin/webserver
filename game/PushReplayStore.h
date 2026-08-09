#pragma once

#include "PushReplayCache.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * 跨 Gateway 共享的可靠 Push 回放存储（Redis）。
 * Key 必须含 session_id：新 Session 不继承旧 Session 私有消息。
 */
class PushReplayStore {
public:
    static PushReplayStore &Instance();

    bool InitFromSessionPrefix(const std::string &key_prefix, size_t cap = 64,
                               int ttl_sec = 7200);
    bool Available() const { return available_; }

    /** 原子分配 server_seq 并追加可靠消息；成功返回 seq>0 */
    uint64_t AppendReliable(uint64_t player_id, const std::string &session_id,
                            const std::string &message_type, const std::string &payload);

    bool ReplayAfter(uint64_t player_id, const std::string &session_id, uint64_t last_acked_seq,
                     std::vector<PushReplayEntry> *out, bool *need_snapshot);

    /** 裁剪 seq<=ack_seq；调用方已校验 session/fence/generation */
    bool Ack(uint64_t player_id, const std::string &session_id, uint64_t ack_seq);

    uint64_t CurrentSeq(uint64_t player_id, const std::string &session_id);

    /** 顶号/新 Session：删除旧 session 回放（可选） */
    bool InvalidateSession(uint64_t player_id, const std::string &session_id);

private:
    PushReplayStore() = default;
    std::string SeqKey(uint64_t player_id, const std::string &session_id) const;
    std::string ListKey(uint64_t player_id, const std::string &session_id) const;
    std::string MetaKey(uint64_t player_id, const std::string &session_id) const;

    bool available_ = false;
    std::string key_prefix_ = "gamemesh:dev:";
    size_t cap_ = 64;
    int ttl_sec_ = 7200;
};
