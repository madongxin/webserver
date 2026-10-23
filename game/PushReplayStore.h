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

    bool InitFromSessionPrefix(const std::string &key_prefix, size_t cap = 1024,
                               int ttl_sec = 7200);
    bool Available() const { return available_; }

    /** 原子分配 server_seq 并追加可靠消息；成功返回 seq>0 */
    uint64_t AppendReliable(uint64_t player_id, const std::string &session_id,
                            const std::string &message_type, const std::string &payload);

    /** 仅 INCR 预留 seq（用于先编码 baseline 再写入，避免存后再改 payload） */
    uint64_t ReserveSeq(uint64_t player_id, const std::string &session_id);

    /**
     * 将已预留的 seq 与完整 payload 写入 replay list。
     * 若当前 seq 计数与 reserved 不一致返回 false（产生 gap，由重连检测）。
     */
    bool AppendReserved(uint64_t player_id, const std::string &session_id, uint64_t reserved_seq,
                        const std::string &message_type, const std::string &payload);

    bool ReplayAfter(uint64_t player_id, const std::string &session_id, uint64_t last_acked_seq,
                     std::vector<PushReplayEntry> *out, bool *need_snapshot);

    enum class AckStatus {
        Ok = 0,
        Duplicate = 1,
        Ahead = 2,
        Stale = 3,
        Unavailable = 4,
        Invalid = 5,
        Gap = 6,  // ACK 跳过空洞 / 指向未写入 seq → NEED_SNAPSHOT
    };
    struct AckResult {
        AckStatus status = AckStatus::Invalid;
        std::string error_code;
        uint64_t trimmed_to_seq = 0;
        bool ok() const {
            return status == AckStatus::Ok || status == AckStatus::Duplicate;
        }
    };
    /** 原子校验 current_seq / last_ack 后裁剪；Redis 不可用返回 Unavailable（失败）。 */
    AckResult Ack(uint64_t player_id, const std::string &session_id, uint64_t ack_seq);

    /**
     * 全量快照已经成功交给客户端之后调用。
     * 原子地把 lastAck、snapBaseline 抬到 max(原值, baseline)，并丢掉 seq ≤ 该水位的回放条目。
     * baseline 超过当前 seq 时夹到当前 seq，不会把光标推到尚未分配的序号。
     */
    bool CommitSnapshot(uint64_t player_id, const std::string &session_id, uint64_t baseline);

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
    size_t cap_ = 1024;
    int ttl_sec_ = 7200;
};
