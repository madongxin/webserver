#include "BotLoginWave.h"

#include <cstdio>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL %s\n", msg);
        ++fails;
    }
}

}  // namespace

int main() {
    BotLoginWaveState state;
    BotLoginWaveSummary closed;
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::Start, 1000, 5000, &closed),
           "first start does not close");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::FinishOk, 1800, 5000, &closed),
           "still inside the quiet window");
    Expect(ApplyBotLoginWave(&state, BotLoginWaveEvent::Poll, 1800 + 5000, 5000, &closed),
           "quiet poll closes the wave");
    Expect(closed.bots == 1 && closed.ok == 1 && closed.failed == 0 && closed.elapsed_ms == 800,
           "one bot elapsed is finish minus start");
    Expect(state.started == 0 && state.done == 0, "state resets after close");

    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::Start, 10000, 5000, &closed), "wave2 a");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::Start, 11000, 5000, &closed), "wave2 b");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::FinishFail, 12000, 5000, &closed),
           "one still in flight");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::Poll, 20000, 5000, &closed),
           "inflight blocks close");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::FinishOk, 21000, 5000, &closed),
           "last finish waits for quiet");
    Expect(ApplyBotLoginWave(&state, BotLoginWaveEvent::Poll, 26000, 5000, &closed), "wave2 closes");
    Expect(closed.bots == 2 && closed.ok == 1 && closed.failed == 1 && closed.elapsed_ms == 11000,
           "wave spans first start to last finish");

    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::Start, 30000, 5000, &closed), "wave3");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::FinishOk, 30100, 5000, &closed), "done");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::Start, 34000, 5000, &closed),
           "new start before quiet extends the wave");
    Expect(!ApplyBotLoginWave(&state, BotLoginWaveEvent::Poll, 36000, 5000, &closed),
           "extended wave still open");
    Expect(state.started == 2, "extended wave kept the first bot");

    Expect(IsLoadTestBotDevice("lt-210345862-001"), "load test device");
    Expect(!IsLoadTestBotDevice("phone-1"), "player device is not a bot wave");

    if (fails) {
        std::printf("bot_login_wave_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK bot_login_wave_test\n");
    return 0;
}
