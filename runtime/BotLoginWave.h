#pragma once

#include <cstdint>
#include <string>

/**
 * 一次压测里，全部机器人登录完成的耗时。
 * 登录完成 = 网关这一次登录返回（Auth、领取 Session、BindPlayer 都结束），不含进图。
 * 设备号以 lt- 开头的才计入。两台网关共用同一份记录。
 * 最后一个登录返回后再安静 5 秒、且没有还在登录的机器人，就记一笔。
 */
struct BotLoginWaveState {
    int64_t start_ms = 0;
    int64_t last_finish_ms = 0;
    int started = 0;
    int done = 0;
    int failed = 0;
};

struct BotLoginWaveSummary {
    int bots = 0;
    int ok = 0;
    int failed = 0;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int64_t elapsed_ms = 0;
};

enum class BotLoginWaveEvent { Start, FinishOk, FinishFail, Poll };

inline constexpr int kBotLoginWaveQuietMs = 5000;

inline bool IsLoadTestBotDevice(const std::string &device_id) {
    return device_id.size() >= 3 && device_id.compare(0, 3, "lt-") == 0;
}

/** 返回 true 表示这一笔已经收口，*closed 里是整批耗时。 */
inline bool ApplyBotLoginWave(BotLoginWaveState *state, BotLoginWaveEvent ev, int64_t now_ms,
                              int quiet_ms, BotLoginWaveSummary *closed) {
    if (!state || quiet_ms < 0)
        return false;
    if (ev == BotLoginWaveEvent::Start) {
        if (state->started == 0)
            state->start_ms = now_ms;
        state->started++;
    } else if (ev == BotLoginWaveEvent::FinishOk || ev == BotLoginWaveEvent::FinishFail) {
        if (state->done >= state->started)
            return false;
        state->done++;
        if (ev == BotLoginWaveEvent::FinishFail)
            state->failed++;
        state->last_finish_ms = now_ms;
    }
    if (state->started > 0 && state->started == state->done && state->last_finish_ms > 0 &&
        now_ms - state->last_finish_ms >= quiet_ms) {
        if (closed) {
            closed->bots = state->done;
            closed->failed = state->failed;
            closed->ok = state->done - state->failed;
            closed->start_ms = state->start_ms;
            closed->end_ms = state->last_finish_ms;
            closed->elapsed_ms = state->last_finish_ms - state->start_ms;
            if (closed->elapsed_ms < 0)
                closed->elapsed_ms = 0;
        }
        *state = BotLoginWaveState{};
        return true;
    }
    return false;
}

/** 记录一次压测机器人登录的开始或结束。非 lt- 设备直接忽略。 */
void NoteLoadTestBotLogin(const std::string &device_id, BotLoginWaveEvent ev);

/** 安静时间到了就收口。网关每秒调用一次。 */
void PollLoadTestBotLoginWave();
