#include "TcpReplySink.h"

#include "Buffer.h"
#include "EventLoop.h"
#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PacketTiming.h"
#include "ProtoFraming.h"
#include "TcpConnection.h"
#include "game.pb.h"

#include <algorithm>
#include <chrono>

namespace {

const char *KindOfResponse(const game::GameResponse &gr) {
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
    LOG_INFO << "control preempted move frames conn#" << c->id() << " moves=" << dropped
             << " before=" << before << " remain=" << c->send_buf()->readablebytes();
}

TcpReplySink::TcpReplySink(const std::shared_ptr<TcpConnection> &conn)
    : conn_(conn), loop_(conn ? conn->loop() : nullptr) {}

void TcpReplySink::SendFrame(const std::string &response_frame) {
    if (!loop_)
        return;
    auto weak = conn_;
    const int64_t enqueued_ms = SteadyMs();
    loop_->QueueOneFunc([weak, response_frame, enqueued_ms]() {
        auto c = weak.lock();
        if (!c)
            return;
        const int64_t wait_ms = SteadyMs() - enqueued_ms;
        if (c->state() != TcpConnection::Connected) {
            LOG_WARN << "TcpReplySink drop frame conn#" << c->id()
                     << " bytes=" << response_frame.size()
                     << " state=" << static_cast<int>(c->state());
            return;
        }
        struct ReplyNote {
            TcpConnection *live = nullptr;
            int64_t wait_ms = 0;
            const std::string *frame = nullptr;
            ~ReplyNote() {
                if (!live || !frame)
                    return;
                game::GameResponse gr;
                std::string buf = *frame;
                std::string payload;
                const char *kind = "other";
                uint64_t seq = 0;
                if (gameproto::DecodeOneFrame(&buf, &payload) ==
                        gameproto::FrameDecodeResult::Complete &&
                    gr.ParseFromString(payload)) {
                    kind = KindOfResponse(gr);
                    seq = gr.seq();
                }
                uint64_t player = 0;
                GatewayConnRegistry::Bind bind;
                if (GatewayConnRegistry::Instance().FindByConnection(live->id(), &bind))
                    player = bind.player_id;
                const int app = live->send_buf() ? live->send_buf()->readablebytes() : 0;
                const int unsent = live->unsent_bytes();
                NoteSendPending(app, unsent);
                NotePktStage(kind, "reply_queue", wait_ms, live->id(), player, seq, app, unsent);
            }
        } reply_note;
        reply_note.live = c.get();
        reply_note.wait_ms = wait_ms;
        reply_note.frame = &response_frame;
        // 登录/重连/进图回包：用满发送缓冲。先把挡在前面的移动包清掉，避免应答排到队尾后客户端超时。
        MakeRoomForControl(c.get());
        // 移动应答（含 ERR_OVERLOADED）必须在客户端丢掉序号之前写出去。
        // 心跳同样抬高上限。全量快照仍走 64KB，避免大帧把缓冲再次堆满。
        bool move_rsp = false;
        {
            std::string buf = response_frame;
            std::string payload;
            game::GameResponse gr;
            move_rsp = gameproto::DecodeOneFrame(&buf, &payload) ==
                           gameproto::FrameDecodeResult::Complete &&
                       gr.ParseFromString(payload) && gr.has_move();
        }
        if (!c->TrySendControl(response_frame, move_rsp)) {
            // 全量快照整帧发不出去时，丢掉排队的移动包再试一次。不改这一帧里的 aoi_entities。
            if (FrameHasFullSnapshot(response_frame)) {
                const size_t dropped = DropQueuedAoiMoveFrames(c.get());
                if (dropped != 0 && c->TrySendControl(response_frame)) {
                    LOG_INFO << "TcpReplySink dropped move frames for full snapshot conn#"
                             << c->id() << " moves=" << dropped
                             << " bytes=" << response_frame.size();
                    OpsMetrics::Instance().AddGatewayTxBytes(response_frame.size());
                    OpsMetrics::Instance().AddGatewayTxFrames(1);
                    return;
                }
                LOG_WARN << "TcpReplySink full snapshot not truncated conn#" << c->id()
                         << " bytes=" << response_frame.size() << " moves_dropped=" << dropped;
            }
            OpsMetrics::Instance().IncReplySoftDrop();
            LOG_WARN << "TcpReplySink soft-drop reply conn#" << c->id()
                     << " bytes=" << response_frame.size();
            return;
        }
        OpsMetrics::Instance().AddGatewayTxBytes(response_frame.size());
        OpsMetrics::Instance().AddGatewayTxFrames(1);
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
