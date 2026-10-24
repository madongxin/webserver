#include "GatewayConnRegistry.h"
#include "OpsMetrics.h"
#include "ReliableOutbox.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

uint64_t Metric(const std::string &text, const std::string &key) {
    size_t pos = 0;
    while (pos < text.size()) {
        const bool line_start = pos == 0 || text[pos - 1] == '\n';
        if (line_start && text[pos] != '#' && text.compare(pos, key.size(), key) == 0) {
            const auto line_end = text.find('\n', pos);
            const auto line =
                text.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
            const auto sp = line.rfind(' ');
            if (sp == std::string::npos)
                return 0;
            return static_cast<uint64_t>(std::strtoull(line.c_str() + sp + 1, nullptr, 10));
        }
        const auto nl = text.find('\n', pos);
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return 0;
}

struct Pipe {
    std::mutex mu;
    std::vector<std::string> sent;
    int fail_left = 0;
    bool closed = false;
    std::function<void()> retry;
};

void BindPipe(uint64_t conn_id, Pipe *pipe) {
    GatewayConnRegistry::Bind bind;
    bind.connection_id = conn_id;
    bind.send_urgent_raw = [pipe](const std::string &frame) -> int {
        std::lock_guard<std::mutex> lk(pipe->mu);
        if (pipe->closed)
            return -1;
        if (pipe->fail_left > 0) {
            --pipe->fail_left;
            return 0;
        }
        pipe->sent.push_back(frame);
        return 1;
    };
    bind.queue_on_loop = [](std::function<void()> fn) {
        if (fn)
            fn();
    };
    bind.run_after = [pipe](double, std::function<void()> cb) { pipe->retry = std::move(cb); };
    bind.close_conn = [pipe]() { pipe->closed = true; };
    GatewayConnRegistry::Instance().Remember(conn_id, std::move(bind));
}

int Push(uint64_t conn_id, uint64_t seq, const std::string &frame) {
    auto order = ReliableOutboxHub::Instance().Lock(conn_id);
    return ReliableOutboxHub::Instance().Accept(order, seq, frame);
}

}  // namespace

int main() {
    {
        ReliableSendQueue q;
        Expect(q.AddFrame(1, "a") == ReliableSendQueue::Add::Head, "first frame is head");
        Expect(q.AddFrame(2, "b") == ReliableSendQueue::Add::Behind, "second frame waits");
        Expect(q.q.front().seq == 1 && q.q.back().seq == 2, "seq order");
        q.MarkHeadFailed();
        Expect(q.HeadFailed(), "head remembers the failed send");
        q.Pop();
        Expect(q.q.front().seq == 2 && !q.HeadFailed(), "pop exposes the next seq");
        ReliableSendQueue big;
        const std::string huge(ReliableSendQueue::kMaxBytes + 1, 'x');
        Expect(big.AddFrame(9, huge) == ReliableSendQueue::Add::Overflow, "1MB cap");
        Expect(big.q.empty(), "overflow does not keep the frame");
        ReliableSendQueue full;
        for (size_t i = 0; i < ReliableSendQueue::kMaxFrames; ++i)
            Expect(full.AddFrame(i + 1, "f") == (i == 0 ? ReliableSendQueue::Add::Head
                                                         : ReliableSendQueue::Add::Behind),
                   "fill to cap");
        Expect(full.AddFrame(999, "f") == ReliableSendQueue::Add::Overflow, "512 cap");
    }

    const auto before = OpsMetrics::Instance().PrometheusText();
    const uint64_t pending_before = Metric(before, "gamemesh_reliable_replay_pending_total{reason=\"buffer_full\"}");
    const uint64_t resend_before = Metric(before, "gamemesh_reliable_resend_total ");
    const uint64_t overflow_before = Metric(before, "gamemesh_reliable_outbox_overflow_total ");

    {
        Pipe pipe;
        pipe.fail_left = 2;
        BindPipe(41, &pipe);
        Expect(Push(41, 10, "A") == 0, "full buffer keeps the head");
        Expect(pipe.sent.empty(), "failed head is not delivered");
        Expect(ReliableOutboxHub::Instance().Depth(41) == 1, "head stays queued");
        Expect(Push(41, 11, "B") == 1, "later reliable frame is queued");
        Expect(pipe.sent.empty(), "later frame does not jump the failed head");
        Expect(ReliableOutboxHub::Instance().Depth(41) == 2, "both frames wait");
        ReliableOutboxHub::Instance().OnBufferDrained(41);
        {
            std::lock_guard<std::mutex> lk(pipe.mu);
            Expect(pipe.sent.empty(), "still full: retry does not skip ahead");
        }
        const auto mid = OpsMetrics::Instance().PrometheusText();
        Expect(Metric(mid, "gamemesh_reliable_replay_pending_total{reason=\"buffer_full\"}") ==
                   pending_before + 1,
               "one buffer_full per frame, not per retry");
        pipe.fail_left = 0;
        ReliableOutboxHub::Instance().OnBufferDrained(41);
        {
            std::lock_guard<std::mutex> lk(pipe.mu);
            Expect(pipe.sent.size() == 2 && pipe.sent[0] == "A" && pipe.sent[1] == "B",
                   "resend is in seq order");
        }
        Expect(ReliableOutboxHub::Instance().Depth(41) == 0, "queue drains");
        const auto after = OpsMetrics::Instance().PrometheusText();
        Expect(Metric(after, "gamemesh_reliable_resend_total ") == resend_before + 1,
               "only the frame that failed is a resend");
        Expect(Metric(after, "gamemesh_reliable_outbox_peak ") >= 2, "peak depth recorded");
        ReliableOutboxHub::Instance().Forget(41);
        GatewayConnRegistry::Instance().Forget(41);
    }

    {
        Pipe pipe;
        pipe.fail_left = 100000;
        BindPipe(42, &pipe);
        Expect(Push(42, 1, "h") == 0, "first of a full queue");
        bool queued = true;
        for (uint64_t seq = 2; seq <= ReliableSendQueue::kMaxFrames; ++seq) {
            if (Push(42, seq, "h") != 1)
                queued = false;
        }
        Expect(queued, "frames up to the cap stay queued");
        Expect(ReliableOutboxHub::Instance().Depth(42) == ReliableSendQueue::kMaxFrames, "at cap");
        Expect(Push(42, 1000, "overflow") == -1, "over cap closes");
        Expect(pipe.closed, "overflow disconnects");
        {
            std::lock_guard<std::mutex> lk(pipe.mu);
            Expect(pipe.sent.empty(), "overflow does not dump the queue onto the socket");
        }
        Expect(Push(42, 1001, "after") == -1, "closed connection accepts nothing more");
        const auto after = OpsMetrics::Instance().PrometheusText();
        Expect(Metric(after, "gamemesh_reliable_outbox_overflow_total ") == overflow_before + 1,
               "overflow counted");
        ReliableOutboxHub::Instance().Forget(42);
        GatewayConnRegistry::Instance().Forget(42);
    }

    {
        Pipe pipe;
        BindPipe(43, &pipe);
        std::atomic<uint64_t> next{1};
        auto worker = [&]() {
            for (int i = 0; i < 40; ++i) {
                auto order = ReliableOutboxHub::Instance().Lock(43);
                const uint64_t seq = next.fetch_add(1);
                ReliableOutboxHub::Instance().Accept(order, seq, std::to_string(seq));
            }
        };
        std::thread a(worker);
        std::thread b(worker);
        a.join();
        b.join();
        std::vector<uint64_t> seqs;
        {
            std::lock_guard<std::mutex> lk(pipe.mu);
            for (const auto &frame : pipe.sent)
                seqs.push_back(static_cast<uint64_t>(std::strtoull(frame.c_str(), nullptr, 10)));
        }
        Expect(seqs.size() == 80, "every reliable frame is sent");
        bool ordered = seqs.size() == 80;
        for (size_t i = 0; i < seqs.size(); ++i) {
            if (seqs[i] != i + 1)
                ordered = false;
        }
        Expect(ordered, "concurrent accepts stay in seq order");
        Expect(ReliableOutboxHub::Instance().Depth(43) == 0, "concurrent queue drains");
        ReliableOutboxHub::Instance().Forget(43);
        GatewayConnRegistry::Instance().Forget(43);
    }

    if (fails != 0) {
        std::printf("reliable_outbox_test FAIL %d\n", fails);
        return 1;
    }
    std::printf("reliable_outbox_test OK\n");
    return 0;
}
