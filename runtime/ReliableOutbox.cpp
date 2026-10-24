#include "ReliableOutbox.h"

#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "OpsMetrics.h"

#include <unordered_map>
#include <utility>

#ifdef WEBSERVER_ENABLE_BRPC
#include <bthread/countdown_event.h>
#include <butil/time.h>
#endif

namespace {

struct Box {
    std::mutex mu;
    ReliableSendQueue q;
    bool flushing = false;
    bool blocked = false;
    bool closed = false;
    bool retry_armed = false;
    bool inside = false;
};

std::mutex g_map_mu;
std::unordered_map<uint64_t, std::shared_ptr<Box>> g_boxes;

std::shared_ptr<Box> FindOrCreate(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_map_mu);
    auto &slot = g_boxes[conn_id];
    if (!slot)
        slot = std::make_shared<Box>();
    return slot;
}

std::shared_ptr<Box> Find(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_map_mu);
    auto it = g_boxes.find(conn_id);
    if (it == g_boxes.end())
        return nullptr;
    return it->second;
}

void NotePeak(size_t depth) {
    OpsMetrics::Instance().NoteReliableOutboxDepth(static_cast<int64_t>(depth));
}

}  // namespace

ReliableOutboxHub &ReliableOutboxHub::Instance() {
    static ReliableOutboxHub hub;
    return hub;
}

ReliableOutboxHub::Order ReliableOutboxHub::Lock(uint64_t conn_id) {
    Order order;
    order.conn_id = conn_id;
    auto box = FindOrCreate(conn_id);
    order.keep = std::shared_ptr<void>(box, box.get());
    order.lk = std::unique_lock<std::mutex>(box->mu);
    order.q = &box->q;
    order.flushing = &box->flushing;
    order.blocked = &box->blocked;
    order.closed = &box->closed;
    return order;
}

int ReliableOutboxHub::Drain(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return -1;
    std::unique_lock<std::mutex> lk(box->mu);
    if (box->closed)
        return -1;
    if (box->inside)
        return 1;
    box->inside = true;
    box->retry_armed = false;
    int first = 1;
    bool saw_first = false;
    GatewayConnRegistry::Bind bind;
    if (!GatewayConnRegistry::Instance().FindByConnection(conn_id, &bind) || !bind.send_urgent_raw) {
        box->inside = false;
        box->flushing = false;
        box->blocked = true;
        return -1;
    }
    auto try_send = bind.send_urgent_raw;
    while (!box->q.q.empty()) {
        ReliableSendQueue::Item item = box->q.q.front();
        lk.unlock();
        const int code = try_send(item.frame);
        lk.lock();
        if (box->closed)
            break;
        if (box->q.q.empty() || box->q.q.front().seq != item.seq ||
            box->q.q.front().frame != item.frame) {
            continue;
        }
        if (code <= 0) {
            const bool first_fail = !box->q.HeadFailed();
            box->q.MarkHeadFailed();
            box->blocked = true;
            box->flushing = false;
            box->inside = false;
            if (first_fail)
                OpsMetrics::Instance().IncReliableReplayPending(code < 0 ? "closed" : "buffer_full");
            if (!saw_first)
                first = code < 0 ? -1 : 0;
            const bool arm = !box->retry_armed && code == 0;
            if (arm)
                box->retry_armed = true;
            lk.unlock();
            if (arm) {
                GatewayConnRegistry::Bind again;
                if (GatewayConnRegistry::Instance().FindByConnection(conn_id, &again) &&
                    again.run_after) {
                    again.run_after(0.2, [conn_id]() {
                        ReliableOutboxHub::Instance().OnBufferDrained(conn_id);
                    });
                }
            }
            return first;
        }
        if (item.failed)
            OpsMetrics::Instance().IncReliableResend();
        box->q.Pop();
        box->blocked = false;
        NotePeak(box->q.q.size());
        if (!saw_first)
            first = 1;
        saw_first = true;
    }
    box->flushing = false;
    if (box->q.q.empty())
        box->blocked = false;
    box->inside = false;
    return first;
}

int ReliableOutboxHub::Accept(Order &order, uint64_t seq, std::string frame) {
    if (!order.q || !order.lk.owns_lock())
        return -1;
    if (order.closed && *order.closed)
        return -1;
    const auto added = order.q->AddFrame(seq, std::move(frame));
    NotePeak(order.q->q.size());
    if (added == ReliableSendQueue::Add::Overflow) {
        LOG_WARN << "reliable outbox overflow conn#" << order.conn_id
                 << " depth=" << order.q->q.size() << " bytes=" << order.q->bytes
                 << " seq=" << seq;
        OpsMetrics::Instance().IncReliableReplayPending("overflow");
        OpsMetrics::Instance().IncReliableOutboxOverflow();
        if (order.closed)
            *order.closed = true;
        const uint64_t conn_id = order.conn_id;
        order.lk.unlock();
        GatewayConnRegistry::Bind bind;
        if (GatewayConnRegistry::Instance().FindByConnection(conn_id, &bind) && bind.close_conn)
            bind.close_conn();
        order.lk.lock();
        return -1;
    }
    // 队里已经有更早的可靠帧，或正在等连接变可写：新帧只能排在后面。
    if (added == ReliableSendQueue::Add::Behind &&
        ((order.flushing && *order.flushing) || (order.blocked && *order.blocked))) {
        return 1;
    }
    if (order.flushing)
        *order.flushing = true;
    const uint64_t conn_id = order.conn_id;
    order.lk.unlock();

    int code = 1;
    GatewayConnRegistry::Bind bind;
    if (!GatewayConnRegistry::Instance().FindByConnection(conn_id, &bind) || !bind.queue_on_loop ||
        !bind.send_urgent_raw) {
        code = -1;
        order.lk.lock();
        if (order.flushing)
            *order.flushing = false;
        return code;
    } else if (bind.queue_on_loop) {
#ifdef WEBSERVER_ENABLE_BRPC
        auto done = std::make_shared<bthread::CountdownEvent>();
        auto stored = std::make_shared<int>(1);
        bind.queue_on_loop([conn_id, done, stored]() {
            *stored = ReliableOutboxHub::Instance().Drain(conn_id);
            done->signal();
        });
        if (done->timed_wait(butil::milliseconds_from_now(200)) != 0) {
            OpsMetrics::Instance().IncPushSendDeferred();
            code = 1;
        } else {
            code = *stored;
        }
#else
        auto stored = std::make_shared<int>(1);
        bind.queue_on_loop([conn_id, stored]() {
            *stored = ReliableOutboxHub::Instance().Drain(conn_id);
        });
        code = *stored;
#endif
    }
    order.lk.lock();
    return code;
}

void ReliableOutboxHub::OnBufferDrained(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return;
    {
        std::lock_guard<std::mutex> lk(box->mu);
        if (box->inside || box->closed || box->q.q.empty())
            return;
        box->retry_armed = false;
        box->flushing = true;
    }
    Drain(conn_id);
}

size_t ReliableOutboxHub::Depth(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return 0;
    std::lock_guard<std::mutex> lk(box->mu);
    if (box->closed)
        return 0;
    return box->q.q.size();
}

void ReliableOutboxHub::Forget(uint64_t conn_id) {
    std::shared_ptr<Box> box;
    {
        std::lock_guard<std::mutex> lk(g_map_mu);
        auto it = g_boxes.find(conn_id);
        if (it == g_boxes.end())
            return;
        box = it->second;
        g_boxes.erase(it);
    }
    std::lock_guard<std::mutex> lk(box->mu);
    box->q.q.clear();
    box->q.bytes = 0;
    box->closed = true;
    box->flushing = false;
    box->blocked = false;
}
