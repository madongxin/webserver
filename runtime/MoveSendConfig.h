#pragma once

#include "ControlResponseOutbox.h"

#include <cstdlib>

/** 发送和读侧阈值。未设置环境变量时用默认值，并在名字里标成待压测校准。 */
inline int MoveEnvInt(const char *name, int fallback, int min_v, int max_v) {
    const char *env = std::getenv(name);
    int v = (env && *env) ? std::atoi(env) : fallback;
    if (v < min_v)
        v = min_v;
    if (v > max_v)
        v = max_v;
    return v;
}

inline int SlowDegradeMs() { return MoveEnvInt("GAMEMESH_SLOW_DEGRADE_MS", 10000, 1000, 600000); }
inline int SlowClearMs() { return MoveEnvInt("GAMEMESH_SLOW_CLEAR_MS", 30000, 1000, 600000); }
inline int SlowCloseMs() { return MoveEnvInt("GAMEMESH_SLOW_CLOSE_MS", 90000, 1000, 600000); }
inline int NearQuotaPercent() { return MoveEnvInt("GAMEMESH_NEAR_QUOTA_PERCENT", 60, 0, 100); }
inline int NearHardSilenceMs() { return MoveEnvInt("GAMEMESH_NEAR_SILENCE_MS", 3000, 200, 120000); }
inline int FarHardSilenceMs() { return MoveEnvInt("GAMEMESH_FAR_SILENCE_MS", 15000, 200, 120000); }
/** 远层（水平距离 ≥ 16 米）两次位置广播的最小间隔。待压测校准。
 *  3 秒太稀，满员时视野里的人看起来是站着。500 毫秒仍低于近处的 200 毫秒。
 *  静默达到 FarHardSilenceMs 时仍会补发。 */
inline int FarMoveIntervalMs() { return MoveEnvInt("GAMEMESH_FAR_MOVE_INTERVAL_MS", 500, 200, 120000); }
/** 只有显式 GAMEMESH_AOI_EXTRAPOLATE=1 才按速度扣广播。默认关，近处 200ms、远处按上面的间隔发坐标。 */
inline bool AoiExtrapolateEnabled() {
    const char *env = std::getenv("GAMEMESH_AOI_EXTRAPOLATE");
    return env && env[0] == '1' && env[1] == '\0';
}
/** 近处移动实体的关键帧间隔。待压测校准。 */
inline int ExtrapKeyframeMs() { return MoveEnvInt("GAMEMESH_EXTRAP_KEYFRAME_MS", 4000, 3000, 5000); }
/** 外推误差超过这么多米才补发。待压测校准。 */
inline int ExtrapErrorCm() { return MoveEnvInt("GAMEMESH_EXTRAP_ERROR_CM", 100, 10, 10000); }
inline int MoveBatchMinEvents() { return MoveEnvInt("GAMEMESH_MOVE_BATCH_MIN", 8, 1, 40); }
inline int FrameCapPreLogin() { return MoveEnvInt("GAMEMESH_FRAME_CAP_PRELOGIN", 16 * 1024, 1024, 1024 * 1024); }
inline int FrameCapLogin() { return MoveEnvInt("GAMEMESH_FRAME_CAP_LOGIN", 64 * 1024, 1024, 4 * 1024 * 1024); }
inline int FramesPerWake() { return MoveEnvInt("GAMEMESH_FRAMES_PER_WAKE", 64, 1, 1024); }
inline int ReadPauseHighPercent() { return MoveEnvInt("GAMEMESH_READ_PAUSE_HIGH_PERCENT", 50, 1, 100); }
inline int ReadPauseLowPercent() { return MoveEnvInt("GAMEMESH_READ_PAUSE_LOW_PERCENT", 25, 0, 99); }
inline int ReadPauseMaxMs() { return MoveEnvInt("GAMEMESH_READ_PAUSE_MAX_MS", 3000, 100, 60000); }

inline bool ReplyQueueHigh(size_t depth, size_t bytes) {
    const size_t high_frames =
        ControlResponseOutbox::kMaxFrames * static_cast<size_t>(ReadPauseHighPercent()) / 100;
    const size_t high_bytes =
        ControlResponseOutbox::kMaxBytes * static_cast<size_t>(ReadPauseHighPercent()) / 100;
    return depth >= high_frames || bytes >= high_bytes;
}

/** 位置帧已因内核未发送队列超过 16KB 停放时，大的可靠帧和快照先别再写进内核。
 *  心跳和请求回包不走这里。512 字节及以下的小帧仍然放行。 */
inline bool DeferBulkWhileKernelParked(bool parked_over_16kb, int nbytes) {
    return parked_over_16kb && nbytes > 512;
}

inline bool ReplyQueueLow(size_t depth, size_t bytes) {
    const size_t low_frames =
        ControlResponseOutbox::kMaxFrames * static_cast<size_t>(ReadPauseLowPercent()) / 100;
    const size_t low_bytes =
        ControlResponseOutbox::kMaxBytes * static_cast<size_t>(ReadPauseLowPercent()) / 100;
    return depth <= low_frames && bytes <= low_bytes;
}
