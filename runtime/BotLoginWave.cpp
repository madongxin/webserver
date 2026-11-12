#include "BotLoginWave.h"

#include "Logging.h"

#include <cerrno>
#include <chrono>
#include <cstdio>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char *kStatePath = "run/formal/bot_login_wave.state";
constexpr const char *kLogPath = "run/formal/bot_login_waves.log";

int64_t NowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool ReadState(FILE *fp, BotLoginWaveState *state) {
    if (!fp || !state)
        return false;
    std::rewind(fp);
    long long start = 0, last = 0;
    int started = 0, done = 0, failed = 0;
    const int n = std::fscanf(fp, "%lld %lld %d %d %d", &start, &last, &started, &done, &failed);
    if (n != 5)
        *state = BotLoginWaveState{};
    else {
        state->start_ms = static_cast<int64_t>(start);
        state->last_finish_ms = static_cast<int64_t>(last);
        state->started = started;
        state->done = done;
        state->failed = failed;
    }
    return true;
}

bool WriteState(FILE *fp, const BotLoginWaveState &state) {
    if (!fp)
        return false;
    std::clearerr(fp);
    std::fflush(fp);
    if (::ftruncate(::fileno(fp), 0) != 0)
        return false;
    std::rewind(fp);
    std::fprintf(fp, "%lld %lld %d %d %d\n", static_cast<long long>(state.start_ms),
                 static_cast<long long>(state.last_finish_ms), state.started, state.done,
                 state.failed);
    std::fflush(fp);
    return true;
}

void AppendSummary(const BotLoginWaveSummary &closed) {
    FILE *fp = std::fopen(kLogPath, "a");
    if (!fp)
        return;
    std::fprintf(fp,
                 "bot_login_wave bots=%d ok=%d fail=%d elapsed_ms=%lld start_ms=%lld end_ms=%lld\n",
                 closed.bots, closed.ok, closed.failed, static_cast<long long>(closed.elapsed_ms),
                 static_cast<long long>(closed.start_ms), static_cast<long long>(closed.end_ms));
    std::fclose(fp);
    LOG_INFO << "bot login wave bots=" << closed.bots << " ok=" << closed.ok
             << " fail=" << closed.failed << " elapsed_ms=" << closed.elapsed_ms;
}

void ApplyLocked(BotLoginWaveEvent ev, int64_t now_ms) {
    if (::mkdir("run", 0755) != 0 && errno != EEXIST)
        return;
    if (::mkdir("run/formal", 0755) != 0 && errno != EEXIST)
        return;
    const int fd = ::open(kStatePath, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return;
    }
    FILE *fp = ::fdopen(fd, "r+");
    if (!fp) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return;
    }
    BotLoginWaveState state;
    ReadState(fp, &state);
    const BotLoginWaveState before = state;
    BotLoginWaveSummary closed;
    const bool finished =
        ApplyBotLoginWave(&state, ev, now_ms, kBotLoginWaveQuietMs, &closed);
    const bool changed = finished || state.started != before.started || state.done != before.done ||
                         state.failed != before.failed;
    if (changed)
        WriteState(fp, state);
    if (finished)
        AppendSummary(closed);
    ::flock(fd, LOCK_UN);
    std::fclose(fp);
}

}  // namespace

void NoteLoadTestBotLogin(const std::string &device_id, BotLoginWaveEvent ev) {
    if (!IsLoadTestBotDevice(device_id))
        return;
    if (ev != BotLoginWaveEvent::Start && ev != BotLoginWaveEvent::FinishOk &&
        ev != BotLoginWaveEvent::FinishFail)
        return;
    ApplyLocked(ev, NowUnixMs());
}

void PollLoadTestBotLoginWave() { ApplyLocked(BotLoginWaveEvent::Poll, NowUnixMs()); }
