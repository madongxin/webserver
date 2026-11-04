#include "ControlResponseOutbox.h"
#include "GatewayConnGuard.h"
#include "InboundFrameBatch.h"
#include "MoveSendConfig.h"
#include "ProtoFraming.h"
#include "PublicError.h"
#include "AoiStrip.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_fail = 0;
void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++g_fail;
    }
}
}

int main() {
    const size_t high = ControlResponseOutbox::kMaxFrames * 50 / 100;
    Expect(ReplyQueueHigh(high, 0), "half the reply queue is the high watermark");
    Expect(!ReplyQueueHigh(high - 1, 0), "one frame under the watermark still reads");
    int processed_while_high = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!ReplyQueueHigh(high, 0))
            ++processed_while_high;
    }
    Expect(processed_while_high == 0, "a full watermark accepts no further reads");
    int processed_while_low = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!ReplyQueueHigh(0, 0))
            ++processed_while_low;
    }
    Expect(processed_while_low == 1000, "an empty reply queue processes the next thousand");

    ControlSendEnv env;
    env.try_send = [](const std::string &) { return 0; };
    const uint64_t outbox_id = 910001;
    int accepted = 0;
    for (size_t i = 0; i < high; ++i) {
        const int rc = ControlResponseOutbox::Instance().Submit(outbox_id, i, "other", "x", env);
        if (rc > 0)
            ++accepted;
    }
    Expect(ControlResponseOutbox::Instance().Depth(outbox_id) >= high, "blocked replies reach the watermark");
    int accepted_after = 0;
    for (int i = 0; i < 1000; ++i) {
        const int rc =
            ControlResponseOutbox::Instance().Submit(outbox_id, high + static_cast<size_t>(i), "other", "x", env);
        if (rc > 0)
            ++accepted_after;
    }
    std::printf("outbox accepted_before=%d accepted_after_watermark=%d depth=%zu\n", accepted, accepted_after,
                ControlResponseOutbox::Instance().Depth(outbox_id));
    Expect(accepted == 0, "a blocked socket accepts none of the queued replies");
    Expect(accepted_after == 0, "a thousand more requests are not written while the queue is blocked");
    ControlResponseOutbox::Instance().Forget(outbox_id);

    const uint64_t limited = 910002;
    const uint64_t credited = 910003;
    GatewayConnGuard::Instance().OnConnected(limited, -1);
    GatewayConnGuard::Instance().OnConnected(credited, -1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    GatewayConnGuard::Instance().AddIdleCredit(credited, 800);
    Expect(GatewayConnGuard::Instance().IdleExpired(limited, 1000), "idle time without a pause expires");
    Expect(!GatewayConnGuard::Instance().IdleExpired(credited, 1000), "a read pause is deducted from idle time");

    const uint64_t noisy = 910004;
    GatewayConnGuard::Instance().OnConnected(noisy, -1);
    std::string err_frame;
    Expect(gameproto::EncodePublicErrorFrame("ERR_RATE_LIMITED", "frame rate limited", 0, noisy, &err_frame),
           "error frame encodes");
    int error_replies = 0;
    size_t error_bytes = 0;
    int limited_frames = 0;
    for (int i = 0; i < 1000; ++i) {
        if (GatewayConnGuard::Instance().CheckFrameRate(noisy, 32).empty())
            continue;
        ++limited_frames;
        if (GatewayConnGuard::Instance().AllowRateError(noisy)) {
            ++error_replies;
            error_bytes += err_frame.size();
        }
    }
    std::printf("rate limit frames=%d error_replies=%d error_bytes=%zu frame_bytes=%zu\n", limited_frames,
                error_replies, error_bytes, err_frame.size());
    Expect(limited_frames > 800, "a thousand frames in one second is over the limit");
    Expect(error_replies == 1, "only one error reply is emitted in that second");
    Expect(error_bytes == err_frame.size(), "error bytes stay at a single frame");

    std::string batch;
    for (int i = 0; i < 200; ++i) {
        game::GameRequest req;
        if (i < 40)
            req.mutable_heartbeat();
        else
            req.mutable_move();
        std::string body;
        std::string frame;
        Expect(req.SerializeToString(&body), "request serializes");
        Expect(gameproto::EncodeFrame(body, &frame), "request frames");
        batch.append(frame);
    }
    std::vector<InboundFrame> frames;
    size_t consumed = 0;
    size_t parses = 0;
    const auto t0 = std::chrono::steady_clock::now();
    Expect(DecodeInboundFrames(batch, 256, &frames, &consumed, &parses), "batch decodes");
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf("batch frames=%zu parses=%zu us=%lld\n", frames.size(), parses, static_cast<long long>(us));
    Expect(frames.size() == 200, "two hundred frames are split");
    Expect(parses == 200, "each frame is parsed once");
    Expect(frames.front().rank == 0, "a heartbeat ranks ahead of a move");
    int heartbeats = 0;
    for (const auto &frame : frames) {
        if (frame.rank == 0)
            ++heartbeats;
    }
    Expect(heartbeats == 40, "heartbeat rank is cached on the frame");

    game::GameResponse leave;
    auto *ev = leave.mutable_aoi_delta()->add_events();
    ev->set_op(3);
    ev->mutable_entity()->set_player_id(7);
    std::string leave_body;
    std::string leave_frame;
    Expect(leave.SerializeToString(&leave_body) && gameproto::EncodeFrame(leave_body, &leave_frame),
           "leave frame encodes");
    const auto ids = AoiStripEntityIds(leave_frame);
    Expect(ids.size() == 1 && ids[0] == 7, "leave names the entity to strip");

    if (g_fail) {
        std::printf("FAIL recv_policy_test failures=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS recv_policy_test\n");
    return 0;
}
