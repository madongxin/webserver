#pragma once

#include "MoveSendConfig.h"

#include <cstdint>

/**
 * 慢客户端只看「多久没有字节写进内核」。
 * 回包不走这里，连接在清槽之后仍然可以回心跳。
 *
 * 第 3 级（默认 90 秒没有任何字节写进内核）和空闲超时是两件事。
 * 本仓库服务端空闲超时默认 45 秒（GAMEMESH_IDLE_TIMEOUT_MS），正式启动脚本把它放到 1 小时。
 * 客户端心跳间隔默认 15 秒。代码里没有单独的 12 秒心跳超时。
 * 对端完全不发字节时，空闲超时会先断开（默认 45 秒，正式环境要看脚本里的覆盖值）。
 * 第 3 级覆盖的是：客户端还在发心跳（空闲计时被刷新），但一个字节都写不进内核。
 */
struct SlowClientConfig {
    int64_t degrade_ms = SlowDegradeMs();
    int64_t clear_ms = SlowClearMs();
    int64_t close_ms = SlowCloseMs();
};

enum class SlowClientAction {
    None = 0,
    Degrade,    // 名额减半，帧间隔拉到 1 秒
    ClearSlot,  // 清空槽、丢弃停放帧，标记需要补发快照
    Close       // CloseForBackpressure("slow_client")
};

class SlowClientTracker {
public:
    void SetConfig(const SlowClientConfig &cfg) { cfg_ = cfg; }
    const SlowClientConfig &config() const { return cfg_; }

    void NoteAnyByteWritten(int64_t now) {
        seen_ = true;
        last_any_ms_ = now;
        if (level_ == 3)
            level_ = 0;
    }

    void NoteMoveWritten(int64_t now) {
        NoteAnyByteWritten(now);
        last_move_ms_ = now;
        if (level_ >= 1 && level_ < 3)
            level_ = 0;
        if (needs_resync_)
            snapshot_due_ = true;
    }

    void NoteSnapshotSent() {
        needs_resync_ = false;
        snapshot_due_ = false;
    }

    /** 这轮没写是预算或发送间隔，不是对端收不动。不推进断开计时。 */
    void NoteServerDeferred(int64_t now) {
        seen_ = true;
        if (last_move_ms_ < now)
            last_move_ms_ = now;
    }

    bool needs_resync() const { return needs_resync_; }
    bool snapshot_due() const { return snapshot_due_; }
    bool degraded() const { return level_ == 1; }

    SlowClientAction Evaluate(int64_t now, bool control_queue_overflow) {
        if (!seen_) {
            seen_ = true;
            last_any_ms_ = now;
            last_move_ms_ = now;
        }
        const int64_t no_any = now - last_any_ms_;
        const int64_t no_move = now - last_move_ms_;
        if (control_queue_overflow || no_any >= cfg_.close_ms) {
            if (level_ < 3) {
                level_ = 3;
                return SlowClientAction::Close;
            }
            return SlowClientAction::None;
        }
        if (no_move >= cfg_.clear_ms) {
            if (level_ < 2) {
                level_ = 2;
                needs_resync_ = true;
                return SlowClientAction::ClearSlot;
            }
            return SlowClientAction::None;
        }
        if (no_move >= cfg_.degrade_ms) {
            if (level_ < 1) {
                level_ = 1;
                return SlowClientAction::Degrade;
            }
            return SlowClientAction::None;
        }
        return SlowClientAction::None;
    }

private:
    SlowClientConfig cfg_;
    bool seen_ = false;
    int64_t last_any_ms_ = 0;
    int64_t last_move_ms_ = 0;
    int level_ = 0;
    bool needs_resync_ = false;
    bool snapshot_due_ = false;
};
