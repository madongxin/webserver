#include "AoiEvidence.h"

#include "ProtoFraming.h"
#include "game.pb.h"

#include <chrono>
#include <string>

void AoiNoteFrameWrite(uint64_t conn_id, const std::string &frame, bool ok, AoiWriteFailReason reason,
                       bool heartbeat) {
    if (heartbeat) {
        if (ok)
            AoiNoteTxBytes(AoiWriteFrame::Heartbeat, frame.size(), conn_id);
        else
            AoiNoteWriteFail(conn_id, AoiWriteFrame::Heartbeat, reason);
        return;
    }
    std::string buf = frame;
    std::string payload;
    game::GameResponse gr;
    if (gameproto::DecodeOneFrame(&buf, &payload) != gameproto::FrameDecodeResult::Complete ||
        !gr.ParseFromString(payload)) {
        if (!ok)
            AoiNoteWriteFail(conn_id, AoiWriteFrame::Other, reason);
        return;
    }
    if (gr.has_full_snapshot()) {
        if (ok)
            AoiNoteTxBytes(AoiWriteFrame::Snapshot, frame.size(), conn_id);
        else
            AoiNoteWriteFail(conn_id, AoiWriteFrame::Snapshot, reason);
        AoiNoteCoincidentEnter(false, true);
        return;
    }
    if (gr.has_heartbeat()) {
        if (ok)
            AoiNoteTxBytes(AoiWriteFrame::Heartbeat, frame.size(), conn_id);
        else
            AoiNoteWriteFail(conn_id, AoiWriteFrame::Heartbeat, reason);
        return;
    }
    const game::AoiDelta *delta = gr.has_aoi_delta() ? &gr.aoi_delta() : nullptr;
    if (!delta || delta->events_size() == 0) {
        if (ok)
            AoiNoteTxBytes(AoiWriteFrame::Reply, frame.size(), conn_id);
        else
            AoiNoteWriteFail(conn_id, AoiWriteFrame::Reply, reason);
        return;
    }
    bool any_enter = false;
    bool any_leave = false;
    bool any_move = false;
    for (const auto &ev : delta->events()) {
        AoiWriteFrame kind = AoiWriteFrame::Move;
        if (ev.op() == 1) {
            kind = AoiWriteFrame::Enter;
            any_enter = true;
        } else if (ev.op() == 3) {
            kind = AoiWriteFrame::Leave;
            any_leave = true;
        } else {
            any_move = true;
        }
        if (!ok)
            AoiNoteWriteFail(conn_id, kind, reason);
        if (AoiTraceEnabled()) {
            AoiTraceFields tr;
            tr.viewer = 0;
            tr.subject = ev.entity().player_id();
            tr.op = ev.op();
            tr.version = ev.entity().state_seq();
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
            if (ok)
                tr.buffer_ms = now;
            else if (reason == AoiWriteFailReason::TimedWait)
                tr.fail = 2;
            else if (reason == AoiWriteFailReason::Closed)
                tr.fail = 3;
            else
                tr.fail = 1;
            AoiTrace(tr);
        }
    }
    if (ok) {
        const AoiWriteFrame kind = any_leave ? AoiWriteFrame::Leave
                                             : (any_enter ? AoiWriteFrame::Enter : AoiWriteFrame::Move);
        if (any_leave || any_enter || any_move)
            AoiNoteTxBytes(kind, frame.size(), conn_id);
    }
    if (any_enter)
        AoiNoteCoincidentEnter(true, false);
}
