#include "TcpReplySink.h"

#include "Buffer.h"
#include "ControlResponseOutbox.h"
#include "EventLoop.h"
#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PacketTiming.h"
#include "ProtoFraming.h"
#include "ReplyStage.h"
#include "TcpConnection.h"
#include "game.pb.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace {

const char *KindOfResponse(const game::GameResponse &gr) {
    if (gr.has_server_hello())
        return "hello";
    if (gr.has_heartbeat())
        return "heartbeat";
    if (gr.has_reconnect())
        return "reconnect";
    if (gr.has_login())
        return "login";
    if (gr.has_register_())
        return "register";
    if (gr.has_enter_map())
        return "enter_map";
    if (gr.has_query_map_lines())
        return "query_lines";
    if (gr.has_move())
        return "move";
    if (gr.has_full_snapshot())
        return "snapshot";
    if (gr.has_friend_list())
        return "friend";
    if (gr.has_push_ack())
        return "push_ack";
    return "other";
}

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool FrameHasFullSnapshot(const std::string &frame) {
    std::string buf = frame;
    std::string payload;
    if (gameproto::DecodeOneFrame(&buf, &payload) != gameproto::FrameDecodeResult::Complete)
        return false;
    game::GameResponse gr;
    return gr.ParseFromString(payload) && gr.has_full_snapshot();
}

}  // namespace

// 写缓冲里只含移动的 AOI 帧可以丢。全量快照的 aoi_entities 不能被截成空列表。
size_t DropQueuedAoiMoveFrames(TcpConnection *c, bool keep_newest) {
    if (!c)
        return 0;
    std::string raw = c->CopySendBuf();
    if (raw.empty())
        return 0;
    const size_t prefix = std::min(c->send_partial_prefix(), raw.size());
    raw.erase(0, prefix);
    if (raw.empty())
        return 0;
    std::string keep;
    std::string newest_move;
    size_t dropped = 0;
    while (!raw.empty()) {
        std::string payload;
        const auto dr = gameproto::DecodeOneFrame(&raw, &payload);
        if (dr != gameproto::FrameDecodeResult::Complete) {
            keep.append(raw);
            break;
        }
        game::GameResponse gr;
        bool drop = false;
        if (gr.ParseFromString(payload) && gr.has_aoi_delta() && gr.aoi_delta().events_size() > 0) {
            drop = true;
            for (const auto &ev : gr.aoi_delta().events()) {
                if (ev.op() != 2) {
                    drop = false;
                    break;
                }
            }
        }
        std::string frame;
        if (!gameproto::EncodeFrame(payload, &frame)) {
            keep.append(raw);
            break;
        }
        if (drop) {
            if (keep_newest) {
                if (!newest_move.empty())
                    ++dropped;
                newest_move = std::move(frame);
            } else {
                ++dropped;
            }
            continue;
        }
        keep.append(frame);
    }
    if (!newest_move.empty())
        keep.append(newest_move);
    if (dropped != 0)
        c->ReplaceQueuedAfterPrefix(keep);
    return dropped;
}

void MakeRoomForControl(TcpConnection *c) {
    if (!c || !c->send_buf())
        return;
    // 心跳和重连回包不能排在移动帧后面。半帧前缀留给 ReplaceQueuedAfterPrefix，避免把已经写出的帧头截断。
    // 最新位置还在连接的合并槽里，这里丢掉的只是还没写进内核的旧移动帧。
    if (c->send_buf()->readablebytes() == 0)
        return;
    const int before = c->send_buf()->readablebytes();
    const size_t dropped = DropQueuedAoiMoveFrames(c, false);
    if (dropped == 0)
        return;
    static std::atomic<uint32_t> logs{0};
    if ((logs.fetch_add(1, std::memory_order_relaxed) & 255) == 0) {
        LOG_INFO << "control preempted move frames conn#" << c->id() << " moves=" << dropped
                 << " before=" << before << " remain=" << c->send_buf()->readablebytes();
    }
}

int SubmitControlOnLoop(const std::shared_ptr<TcpConnection> &conn, uint64_t seq, const char *kind,
                        const std::string &frame) {
    if (!conn || conn->state() != TcpConnection::Connected || frame.empty())
        return -1;
    std::weak_ptr<TcpConnection> weak = conn;
    ControlSendEnv env;
    env.try_send = [weak](const std::string &pending) -> int {
        auto live = weak.lock();
        if (!live || live->state() != TcpConnection::Connected)
            return -1;
        // 回包插到缓冲里的 MOVE 前面，不先把 MOVE 丢掉。只有这一帧写不进缓冲时才清 MOVE。
        // 2 表示帧还在应用缓冲里，调用方必须停住，不能再发下一条。
        auto rc = live->TrySendControl(pending, true);
        if (rc == TcpConnection::Blocked && DropQueuedAoiMoveFrames(live.get()) != 0 &&
            live->state() == TcpConnection::Connected)
            rc = live->TrySendControl(pending, true);
        return static_cast<int>(rc);
    };
    env.drop_moves = [weak]() -> size_t {
        auto live = weak.lock();
        if (!live)
            return 0;
        return DropQueuedAoiMoveFrames(live.get());
    };
    env.close_conn = [weak]() {
        auto live = weak.lock();
        if (!live || live->state() != TcpConnection::Connected)
            return;
        live->HandleClose();
    };
    return ControlResponseOutbox::Instance().Submit(conn->id(), seq, kind ? kind : "other", frame,
                                                    std::move(env));
}

TcpReplySink::TcpReplySink(const std::shared_ptr<TcpConnection> &conn)
    : conn_(conn), loop_(conn ? conn->loop() : nullptr) {}

void TcpReplySink::SendFrame(const std::string &response_frame) {
    uint64_t seq = 0;
    const char *kind = "other";
    {
        std::string buf = response_frame;
        std::string payload;
        game::GameResponse gr;
        if (gameproto::DecodeOneFrame(&buf, &payload) == gameproto::FrameDecodeResult::Complete &&
            gr.ParseFromString(payload)) {
            kind = KindOfResponse(gr);
            seq = gr.seq();
        }
    }
    ReplyStage::Inc(kind, ReplyStage::LogicResponseReady);
    if (!loop_)
        return;
    ReplyStage::Inc(kind, ReplyStage::EventLoopEnqueued);
    auto weak = conn_;
    const int64_t enqueued_ms = SteadyMs();
    const bool snapshot = FrameHasFullSnapshot(response_frame);
    loop_->QueueOneFunc([weak, response_frame, enqueued_ms, seq, kind, snapshot]() {
        auto c = weak.lock();
        if (!c || c->state() != TcpConnection::Connected) {
            LOG_WARN << "TcpReplySink conn gone bytes=" << response_frame.size()
                     << " kind=" << kind << " seq=" << seq;
            return;
        }
        const int64_t wait_ms = SteadyMs() - enqueued_ms;
        uint64_t player = 0;
        GatewayConnRegistry::Bind bind;
        if (GatewayConnRegistry::Instance().FindByConnection(c->id(), &bind))
            player = bind.player_id;
        const int app = c->send_buf() ? c->send_buf()->readablebytes() : 0;
        const int unsent = c->unsent_bytes();
        NoteSendPending(app, unsent);
        NotePktStage(kind, "reply_queue", wait_ms, c->id(), player, seq, app, unsent);
        OpsMetrics::Instance().IncReplyGenerated();
        const int rc = SubmitControlOnLoop(c, seq, kind, response_frame);
        if (rc < 0 && snapshot) {
            LOG_WARN << "TcpReplySink full snapshot not queued conn#" << c->id()
                     << " bytes=" << response_frame.size();
        }
    });
}

void TcpReplySink::CloseConnection() {
    if (!loop_)
        return;
    auto weak = conn_;
    loop_->QueueOneFunc([weak]() {
        auto c = weak.lock();
        if (!c)
            return;
        if (c->state() != TcpConnection::Connected)
            return;
        c->HandleClose();
    });
}

std::shared_ptr<TcpConnection> TcpReplySink::tcp_connection() const {
    return conn_.lock();
}
