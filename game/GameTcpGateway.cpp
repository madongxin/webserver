/**
 * @file GameTcpGateway.cpp
 * @brief 网关：拆帧投递 + Login/Reconnect 本地绑定 + 断线 MarkDisconnected
 */

#include "GameTcpGateway.h"

#include "Buffer.h"
#include "CommandPolicy.h"
#include "EventLoop.h"
#include "FormalMode.h"
#include "GameRequestPlayerId.h"
#include "GameRequestTransport.h"
#include "GatewayAuthFlow.h"
#include "GatewayAuthPolicy.h"
#include "GatewayConnGuard.h"
#include "GatewayConnRegistry.h"
#include "GatewayDisconnectAsync.h"
#include "GatewayIdentity.h"
#include "GatewayLoginRoute.h"
#include "HealthProbe.h"
#include "InProcessTransport.h"
#include "Logging.h"
#include "PacketTiming.h"
#include "LoginAdmission.h"
#include "MapCatalog.h"
#include "OpsMetrics.h"
#include "PlayerSerialQueue.h"
#include "ProtocolHandshake.h"
#include "ProtoFraming.h"
#include "PublicError.h"
#include "ReplySink.h"
#include "RpcOffloadPool.h"
#include "SceneKind.h"
#include "ServiceHealth.h"
#include "SessionHandle.h"
#include "TcpConnection.h"
#include "TcpReplySink.h"
#include "TcpServer.h"
#include "game.pb.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#include "SessionStore.h"
#ifdef WEBSERVER_ENABLE_BRPC
#include "GatewayAuthClients.h"
#include "GatewayEnterMapOrchestrator.h"
#include "GatewayLoginOrchestrator.h"
#include "GatewayLogoutPolicy.h"
#include "SessionRpcClient.h"
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct ConnBind {
    uint64_t player_id = 0;
    std::string token;
    std::string session_id;
    uint64_t generation = 0;
};

std::mutex g_bind_mu;
std::unordered_map<uint64_t, ConnBind> g_conn_bind;  // connection_id -> bind

// 一条连接上的移动推送只留每个实体的最新位置，并且最多每 200ms 写出一帧。
// 否则 100 人挤在同一视野时，每次移动都会往这条连接的事件循环里排一个发送回调，
// 心跳和重连回包排在这些回调后面，要等整段队列跑完才能写到 socket。
struct MoveCoalesceSlot {
    std::mutex mu;
    uint64_t map_instance_id = 0;
    std::unordered_map<uint64_t, game::AoiEvent> by_entity;
    bool scheduled = false;
    int64_t last_flush_ms = 0;
};

std::mutex g_move_slots_mu;
std::unordered_map<uint64_t, std::shared_ptr<MoveCoalesceSlot>> g_move_slots;

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool IsMoveOnlyAoi(const game::GameResponse &gr) {
    if (!gr.has_aoi_delta() || gr.aoi_delta().events_size() <= 0)
        return false;
    for (const auto &ev : gr.aoi_delta().events()) {
        if (ev.op() != 2)
            return false;
    }
    return true;
}

void EraseMoveSlot(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_move_slots_mu);
    g_move_slots.erase(conn_id);
}

void FlushMoveSlot(const std::shared_ptr<MoveCoalesceSlot> &slot,
                   const std::weak_ptr<TcpConnection> &weak, uint64_t conn_id) {
    auto live = weak.lock();
    if (!slot || !live || live->state() != TcpConnection::Connected || !live->loop()) {
        if (slot) {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->by_entity.clear();
            slot->scheduled = false;
        }
        EraseMoveSlot(conn_id);
        return;
    }
    constexpr int64_t kIntervalMs = 200;
    int64_t wait_ms = 0;
    game::AoiDelta delta;
    {
        std::lock_guard<std::mutex> lk(slot->mu);
        const int64_t now = SteadyMs();
        if (slot->last_flush_ms != 0 && now - slot->last_flush_ms < kIntervalMs &&
            !slot->by_entity.empty()) {
            wait_ms = kIntervalMs - (now - slot->last_flush_ms);
        } else {
            delta.set_map_instance_id(slot->map_instance_id);
            for (auto &kv : slot->by_entity)
                *delta.add_events() = std::move(kv.second);
            slot->by_entity.clear();
            slot->last_flush_ms = now;
            slot->scheduled = false;
        }
    }
    if (wait_ms > 0) {
        live->loop()->RunAfter(static_cast<double>(wait_ms) / 1000.0,
                               [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    if (delta.events_size() == 0)
        return;
    const int event_n = delta.events_size();
    auto restore = [&](game::AoiDelta pending) {
        std::lock_guard<std::mutex> lk(slot->mu);
        if (pending.map_instance_id() != 0)
            slot->map_instance_id = pending.map_instance_id();
        for (auto &ev : *pending.mutable_events()) {
            const uint64_t id = ev.entity().player_id();
            if (id == 0)
                continue;
            auto &kept = slot->by_entity[id];
            if (kept.has_entity() && ev.entity().state_seq() < kept.entity().state_seq())
                continue;
            kept = std::move(ev);
        }
        if (!slot->scheduled && !slot->by_entity.empty())
            slot->scheduled = true;
    };
    // 先丢掉缓冲里的旧移动帧，再放这一帧最新位置。
    // 内核里已经有没送出的字节时，TrySendLatestMove 不会再把这一帧写进去。
    // 否则回包排在旧坐标后面，客户端要读完才走下一步，人会越走越慢直到停下。
    DropQueuedAoiMoveFrames(live.get(), false);
    game::GameResponse notify;
    notify.set_ok(true);
    notify.set_message("aoi");
    *notify.mutable_aoi_delta() = delta;
    std::string body;
    std::string frame;
    if (!notify.SerializeToString(&body) || !gameproto::EncodeFrame(body, &frame)) {
        restore(std::move(delta));
        NoteMoveFlush(false, event_n);
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    const bool sent = live->TrySendLatestMove(frame);
    const bool held = sent && live->holding_latest_move();
    if (held)
        NoteMoveHold(event_n);
    else
        NoteMoveFlush(sent, event_n);
    if (live->send_buf())
        NoteSendPending(live->send_buf()->readablebytes(), live->unsent_bytes());
    if (!sent) {
        restore(std::move(delta));
        NoteMovePendingEntities(event_n);
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    NoteMovePendingEntities(0);
    OpsMetrics::Instance().AddGatewayTxBytes(frame.size());
    OpsMetrics::Instance().AddGatewayTxFrames(1);
}

// 纯移动帧被合并后返回 true。ENTER/LEAVE 或解析不了的帧返回 false，调用方照旧立刻发送。
bool OfferCoalescedMove(const std::shared_ptr<TcpConnection> &c, const std::string &frame) {
    if (!c || !c->loop() || frame.empty())
        return false;
    std::string buf = frame;
    std::string payload;
    if (gameproto::DecodeOneFrame(&buf, &payload) != gameproto::FrameDecodeResult::Complete)
        return false;
    game::GameResponse gr;
    if (!gr.ParseFromString(payload) || !IsMoveOnlyAoi(gr))
        return false;
    std::shared_ptr<MoveCoalesceSlot> slot;
    {
        std::lock_guard<std::mutex> lk(g_move_slots_mu);
        auto &found = g_move_slots[c->id()];
        if (!found)
            found = std::make_shared<MoveCoalesceSlot>();
        slot = found;
    }
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lk(slot->mu);
        if (gr.aoi_delta().map_instance_id() != 0)
            slot->map_instance_id = gr.aoi_delta().map_instance_id();
        for (const auto &ev : gr.aoi_delta().events()) {
            const uint64_t id = ev.entity().player_id();
            if (id == 0)
                continue;
            auto &kept = slot->by_entity[id];
            // 序号不增加的移动客户端会整条丢掉。只保留更大的 state_seq。
            if (kept.has_entity() && ev.entity().state_seq() < kept.entity().state_seq())
                continue;
            kept = ev;
        }
        if (slot->by_entity.empty())
            return true;
        if (!slot->scheduled) {
            slot->scheduled = true;
            schedule = true;
        }
        NoteMovePendingEntities(static_cast<int>(slot->by_entity.size()));
    }
    NoteMoveOffer(schedule);
    if (!schedule)
        return true;
    const uint64_t conn_id = c->id();
    std::weak_ptr<TcpConnection> weak = c;
    c->loop()->QueueOneFunc([slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
    return true;
}
std::unordered_set<uint64_t> g_authoritative_logout;  // conn_id：主动 Logout 成功，断线不得 MarkDisconnected
std::string g_gateway_id = "gw-0";
std::atomic<bool> g_tcp_listening{false};

void RememberBind(uint64_t conn_id, uint64_t player_id, const std::string &token,
                  const std::string &session_id, uint64_t generation,
                  std::shared_ptr<ReplySink> sink,
                  const gameproto::GatewayLoginRoute *route = nullptr) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    ConnBind b;
    b.player_id = player_id;
    b.token = token;
    b.session_id = session_id;
    b.generation = generation;
    g_conn_bind[conn_id] = b;

    GatewayConnRegistry::Bind gb;
    gb.player_id = player_id;
    gb.token = token;
    gb.session_id = session_id;
    gb.generation = generation;
    gb.connection_id = conn_id;
    gb.gateway_instance_id = g_gateway_id;
    if (route) {
        gb.gamelogic_instance_id = route->gamelogic_instance_id;
        gb.map_instance_id = route->map_instance_id;
        gb.map_owner_epoch = route->map_owner_epoch;
        gb.route_version = route->route_version;
    }
    if (sink) {
        std::weak_ptr<TcpConnection> weak_conn = sink->tcp_connection();
        auto make_try_send = [weak_conn](bool urgent) {
            return [weak_conn, urgent](const std::string &frame) {
                auto c = weak_conn.lock();
                if (!c || !c->loop())
                    return;
                // 推送路径用 TrySend：队列满丢帧，禁止背压关连接（否则 MarkDisconnected 雪崩）。
                auto do_send = [weak_conn, frame, urgent]() {
                    auto live = weak_conn.lock();
                    if (!live || live->state() != TcpConnection::Connected)
                        return;
                    if (urgent)
                        MakeRoomForControl(live.get());
                    bool ok = urgent ? live->TrySendUrgent(frame) : live->TrySend(frame);
                    // ENTER/LEAVE 发不出去时丢掉排队的移动包再试一次。不改这一帧的内容。
                    if (!ok && urgent) {
                        const size_t dropped = DropQueuedAoiMoveFrames(live.get());
                        if (dropped != 0 && live->TrySendUrgent(frame)) {
                            LOG_INFO << "Gateway dropped move frames for aoi conn#" << live->id()
                                     << " moves=" << dropped << " bytes=" << frame.size();
                            ok = true;
                        }
                    }
                    if (!ok)
                        return;
                    OpsMetrics::Instance().AddGatewayTxBytes(frame.size());
                    OpsMetrics::Instance().AddGatewayTxFrames(1);
                };
                if (c->loop()->IsInLoopThread())
                    do_send();
                else
                    c->loop()->QueueOneFunc(std::move(do_send));
            };
        };
        auto send_move_now = make_try_send(false);
        gb.send_frame = [weak_conn, send_move_now](const std::string &frame) {
            auto c = weak_conn.lock();
            if (!c || !c->loop())
                return;
            if (OfferCoalescedMove(c, frame))
                return;
            send_move_now(frame);
        };
        gb.send_frame_urgent = make_try_send(true);
        gb.close_conn = [weak_conn]() {
            auto c = weak_conn.lock();
            if (!c)
                return;
            TcpReplySink live(c);
            live.CloseConnection();
        };
        gb.run_after = [weak_conn](double delay_sec, std::function<void()> cb) {
            auto c = weak_conn.lock();
            if (!c) {
                if (cb)
                    cb();
                return;
            }
            c->loop()->RunAfter(delay_sec, [weak_conn, cb = std::move(cb)]() {
                auto live = weak_conn.lock();
                if (!live)
                    return;
                if (cb)
                    cb();
            });
        };
        gb.queue_on_loop = [weak_conn](std::function<void()> fn) {
            auto c = weak_conn.lock();
            if (!c || !c->loop()) {
                if (fn)
                    fn();
                return;
            }
            c->loop()->QueueOneFunc(std::move(fn));
        };
    }
    GatewayConnRegistry::Instance().Remember(conn_id, std::move(gb));
    OpsMetrics::Instance().SetOnlinePlayers(GatewayConnRegistry::Instance().BoundPlayerCount());
}

void ForgetBind(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_conn_bind.erase(conn_id);
    GatewayConnRegistry::Instance().Forget(conn_id);
    OpsMetrics::Instance().SetOnlinePlayers(GatewayConnRegistry::Instance().BoundPlayerCount());
}

void MarkAuthoritativeLogout(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_authoritative_logout.insert(conn_id);
    g_conn_bind.erase(conn_id);
    GatewayConnRegistry::Instance().Forget(conn_id);
    OpsMetrics::Instance().SetOnlinePlayers(GatewayConnRegistry::Instance().BoundPlayerCount());
}

bool ConsumeAuthoritativeLogout(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    return g_authoritative_logout.erase(conn_id) > 0;
}

bool HasAuthoritativeLogout(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    return g_authoritative_logout.count(conn_id) > 0;
}

void SendPublicErr(const std::shared_ptr<ReplySink> &sink, uint64_t conn_id, uint64_t seq,
                   const char *code, const char *msg) {
    std::string out;
    if (gameproto::EncodePublicErrorFrame(code, msg, seq, conn_id, &out) && sink)
        sink->SendFrame(out);
}

void ScheduleIdleCheck(std::weak_ptr<TcpConnection> weak, uint64_t id) {
    auto conn = weak.lock();
    if (!conn)
        return;
    conn->loop()->RunAfter(0.5, [weak, id]() {
        auto c = weak.lock();
        if (!c)
            return;
        const bool hello_ok = GatewayConnGuard::Instance().HelloOk(id);
        uint32_t idle_ms = gameproto::IdleTimeoutMs();
        if (!hello_ok && ClientHelloRequired())
            idle_ms = gameproto::HelloDeadlineMs();
        if (idle_ms < 200)
            idle_ms = 200;
        if (GatewayConnGuard::Instance().IdleExpired(id, idle_ms)) {
            OpsMetrics::Instance().IncIdleTimeout();
            LOG_WARN << "Gateway idle timeout conn#" << id << " hello_ok=" << hello_ok;
            c->HandleClose();
            return;
        }
        ScheduleIdleCheck(weak, id);
    });
}

class BindingReplySink : public ReplySink {
public:
    BindingReplySink(std::shared_ptr<ReplySink> inner, uint64_t conn_id, uint64_t player_id,
                     bool is_login_or_reconnect)
        : inner_(std::move(inner)),
          conn_id_(conn_id),
          player_id_(player_id),
          bindable_(is_login_or_reconnect) {}

    void SendFrame(const std::string &response_frame) override {
        if (bindable_ && player_id_ != 0) {
            std::string buf = response_frame;
            std::string payload;
            if (gameproto::DecodeOneFrame(&buf, &payload) == gameproto::FrameDecodeResult::Complete) {
                game::GameResponse rsp;
                if (rsp.ParseFromString(payload) && rsp.ok()) {
                    std::string token;
                    std::string session_id;
                    uint64_t generation = 0;
                    if (rsp.has_login() && rsp.login().ok()) {
                        token = rsp.login().token();
                        session_id = rsp.login().session_id();
                        generation = rsp.login().generation();
                    } else if (rsp.has_reconnect() && rsp.reconnect().ok()) {
                        token = rsp.reconnect().token();
                        session_id = rsp.reconnect().session_id();
                        generation = rsp.reconnect().generation();
                    }
                    if (!token.empty()) {
                        RememberBind(conn_id_, player_id_, token, session_id, generation, inner_);
#ifdef WEBSERVER_ENABLE_REDIS
#ifdef WEBSERVER_ENABLE_BRPC
                        if (SessionRpcClient::Instance().ready()) {
                            SessionRpcClient::Instance().BindConnection(player_id_, token,
                                                                       g_gateway_id, conn_id_);
                        } else
#endif
                            if (SessionStore::Instance().Available()) {
                            SessionStore::Instance().BindConnection(player_id_, token, g_gateway_id,
                                                                   conn_id_);
                        }
#endif
                        LOG_INFO << "Gateway bind conn#" << conn_id_ << " player_id=" << player_id_
                                 << " generation=" << generation;
                    }
                }
            }
        }
        if (inner_)
            inner_->SendFrame(response_frame);
    }

    std::shared_ptr<TcpConnection> tcp_connection() const override {
        return inner_ ? inner_->tcp_connection() : nullptr;
    }

private:
    std::shared_ptr<ReplySink> inner_;
    uint64_t conn_id_ = 0;
    uint64_t player_id_ = 0;
    bool bindable_ = false;
};

}  // namespace

GameTcpGateway::GameTcpGateway(const std::string &ip, int port, const std::string &instance_id)
    : ip_(ip), port_(port), instance_id_(instance_id) {
    if (instance_id_.empty() && GatewayIdentity::Instance().ready())
        instance_id_ = GatewayIdentity::Instance().id();
    if (instance_id_.empty())
        instance_id_ = "gw-0";
    g_gateway_id = instance_id_;
}

bool GameTcpGateway::TcpListening() { return g_tcp_listening.load(std::memory_order_acquire); }

GameTcpGateway::~GameTcpGateway() {
    RequestQuit();
    if (thread_.joinable())
        thread_.join();
}

void GameTcpGateway::StartInBackground() {
    thread_ = std::thread([this]() { Run(); });
}

void GameTcpGateway::StopAccepting() {
    EventLoop *lp = loop_.load();
    TcpServer *srv = server_.load();
    if (!lp || !srv)
        return;
    lp->QueueOneFunc([srv]() { srv->StopAccepting(); });
}

void GameTcpGateway::RequestQuit() {
    EventLoop *lp = loop_.load();
    if (!lp)
        return;
    lp->QueueOneFunc([lp]() { lp->Quit(); });
}

void GameTcpGateway::Run() {
    InProcessTransport::Instance().EnsureStarted(0);
    RpcOffloadPool::Instance().Start(0);
#ifdef WEBSERVER_ENABLE_REDIS
    GatewayDisconnectAsync::Instance().Start(4096);
#endif

    EventLoop loop;
    auto server = std::make_unique<TcpServer>(&loop, ip_.c_str(), port_);
    loop_.store(&loop);
    server_.store(server.get());
    int workers = static_cast<int>(std::thread::hardware_concurrency());
    if (workers <= 1)
        workers = 1;
    else
        --workers;
    server->SetThreadNums(workers);
    server->set_connection_callback([](const std::shared_ptr<TcpConnection> &c) {
        OpsMetrics::Instance().IncTcpConnect();
        if (ServiceHealth::Instance().draining()) {
            OpsMetrics::Instance().IncDrainReject();
            c->HandleClose();
            return;
        }
        const std::string lim = GatewayConnGuard::Instance().CheckConnectRate(c->fd());
        if (!lim.empty()) {
            OpsMetrics::Instance().IncConnRateLimited();
            LOG_WARN << "Gateway connect rate-limited conn#" << c->id();
            c->HandleClose();
            return;
        }
        GatewayConnGuard::Instance().OnConnected(c->id(), c->fd());
        GatewayAuthFlow::Instance().OnConnected(c->id());
        (void)ConsumeAuthoritativeLogout(c->id());
        std::weak_ptr<TcpConnection> weak = c;
        const uint64_t id = c->id();
        c->loop()->QueueOneFunc([weak, id]() { ScheduleIdleCheck(weak, id); });
    });
    server->set_message_callback([this](const std::shared_ptr<TcpConnection> &c) { OnMessage(c); });
    server->set_disconnect_callback([](const std::shared_ptr<TcpConnection> &c) {
        OpsMetrics::Instance().IncTcpDisconnect();
        GatewayAuthFlow::Instance().OnDisconnected(c->id());
        GatewayConnGuard::Instance().OnDisconnected(c->id());
        ConnBind bind;
        {
            std::lock_guard<std::mutex> lk(g_bind_mu);
            auto it = g_conn_bind.find(c->id());
            if (it != g_conn_bind.end()) {
                bind = it->second;
                g_conn_bind.erase(it);
            }
        }
#ifdef WEBSERVER_ENABLE_BRPC
        GatewayConnRegistry::Bind reg;
        const bool taken_over =
            GatewayConnRegistry::Instance().HasNewerBinding(c->id(), bind.player_id);
        const bool has_reg =
            !taken_over && GatewayConnRegistry::Instance().FindByConnection(c->id(), &reg);
#else
        const bool taken_over =
            GatewayConnRegistry::Instance().HasNewerBinding(c->id(), bind.player_id);
#endif
        // 必须在 g_bind_mu 之外 Forget：ForgetBind 会再锁同一把非递归 mutex。
        // 未绑定连接（Register 超时后客户端断开）走旧 early-return 会把 acceptor EventLoop 卡死，
        // listen backlog 堆满后所有新登录 no_response。
        ForgetBind(c->id());

        const bool logout_done = ConsumeAuthoritativeLogout(c->id());
        if (logout_done) {
            // 主动 Logout 已释放 Session；断线不得把 Session 写回 DISCONNECTED。
            return;
        }

        if (taken_over) {
            LOG_INFO << "Gateway disconnect stale conn#" << c->id() << " player_id="
                     << bind.player_id << " generation=" << bind.generation
                     << " (player moved to a newer conn; skip MarkDisconnected/Unbind)";
            return;
        }

        if (bind.player_id != 0) {
#ifdef WEBSERVER_ENABLE_REDIS
#ifdef WEBSERVER_ENABLE_BRPC
            if (SessionRpcClient::Instance().ready()) {
                SessionRpcClient::Instance().MarkDisconnectedAsync(bind.player_id, bind.token,
                                                                   bind.generation);
                OpsMetrics::Instance().IncDisconnectAccepted();
            } else
#endif
                if (SessionStore::Instance().Available()) {
                // 本地 Redis 兜底：后台队列，禁止在 Reactor 同步访问 Redis
                if (GatewayDisconnectAsync::Instance().EnqueueMarkDisconnected(
                        bind.player_id, bind.token, bind.generation)) {
                    OpsMetrics::Instance().IncDisconnectAccepted();
                } else {
#ifdef WEBSERVER_ENABLE_BRPC
                    if (SessionRpcClient::Instance().ready()) {
                        SessionRpcClient::Instance().MarkDisconnectedAsync(
                            bind.player_id, bind.token, bind.generation);
                        OpsMetrics::Instance().IncDisconnectRetried();
                        OpsMetrics::Instance().IncDisconnectAccepted();
                        LOG_WARN << "disconnect queue full, rpc fallback player=" << bind.player_id;
                    } else
#endif
                    {
                        OpsMetrics::Instance().IncDisconnectDropped();
                        OpsMetrics::Instance().IncDisconnectFailed();
                        LOG_WARN << "disconnect redis enqueue failed player=" << bind.player_id;
                    }
                }
            }
#endif
#ifdef WEBSERVER_ENABLE_BRPC
            if (has_reg && !reg.gamelogic_instance_id.empty() &&
                GatewayAuthClients::Instance().ready()) {
                glrpc::UnbindPlayerRequest ureq;
                ureq.set_player_id(bind.player_id);
                ureq.set_session_id(bind.session_id);
                ureq.set_fence_token(bind.token);
                ureq.set_generation(bind.generation);
                ureq.set_reason("tcp_disconnect");
                GatewayAuthClients::Instance().UnbindPlayerAsync(reg.gamelogic_instance_id, ureq);
            }
#endif
            game::GameRequest flush_req;
            flush_req.set_seq(0);
            flush_req.set_session_token(bind.token);
            auto *fb = flush_req.mutable_flush_bag();
            fb->set_player_id(bind.player_id);
            fb->set_reason("disconnect");
            std::string payload;
            if (flush_req.SerializeToString(&payload)) {
                class DropSink : public ReplySink {
                public:
                    void SendFrame(const std::string &) override {}
                };
                SessionHandle h;
                h.player_id = bind.player_id;
                h.connection_id = c->id();
                h.generation = bind.generation;
                h.session_id = bind.session_id;
                h.fence_token = bind.token;
#ifdef WEBSERVER_ENABLE_BRPC
                if (has_reg) {
                    h.gamelogic_instance_id = reg.gamelogic_instance_id;
                    h.map_instance_id = reg.map_instance_id;
                    h.owner_epoch = reg.map_owner_epoch;
                    h.route_version = reg.route_version;
                    if (h.session_id.empty())
                        h.session_id = reg.session_id;
                    if (h.fence_token.empty())
                        h.fence_token = reg.token;
                }
#endif
                // 无 sticky 时跳过 flush，避免 BrpcTransport missing sticky 刷屏。
                if (!h.gamelogic_instance_id.empty()) {
                    GameRequestTransport::Get().PostPlayerRequest(
                        h, std::move(payload), std::make_shared<DropSink>());
                }
            }
        }
        LOG_INFO << "Gateway disconnect conn#" << c->id() << " player_id=" << bind.player_id
                 << " generation=" << bind.generation;
    });
    LOG_INFO << "GameTcpGateway ready on " << ip_ << ":" << port_
             << " id=" << g_gateway_id << " (session bind + grace disconnect)";
    g_tcp_listening.store(true, std::memory_order_release);
    HealthProbeStore::SetGatewayTcpListening(true);
    server->Start();
    g_tcp_listening.store(false, std::memory_order_release);
    HealthProbeStore::SetGatewayTcpListening(false);
    server_.store(nullptr);
    loop_.store(nullptr);
#ifdef WEBSERVER_ENABLE_REDIS
    GatewayDisconnectAsync::Instance().Stop(std::chrono::milliseconds(2000));
#endif
    RpcOffloadPool::Instance().Stop();
}

void GameTcpGateway::OnMessage(const std::shared_ptr<TcpConnection> &conn) {
    Buffer *rb = conn->read_buf();
    if (!rb || rb->readablebytes() <= 0)
        return;

    const size_t nread = static_cast<size_t>(rb->readablebytes());
    OpsMetrics::Instance().AddGatewayRxBytes(nread);

    // 流缓冲挂在连接上，仅由所属 EventLoop 访问，无需全局锁
    std::string &buf = conn->proto_stream();
    buf.append(rb->RetrieveAllAsString());

    std::vector<std::string> frames;
    std::string frame;
    for (;;) {
        const auto dr = gameproto::DecodeOneFrame(&buf, &frame);
        if (dr == gameproto::FrameDecodeResult::Complete) {
            frames.push_back(std::move(frame));
            OpsMetrics::Instance().AddGatewayRxFrames(1);
            frame.clear();
            continue;
        }
        if (dr == gameproto::FrameDecodeResult::Invalid) {
            OpsMetrics::Instance().IncIllegalFrame();
            LOG_WARN << "GameTcpGateway invalid frame conn#" << conn->id()
                     << " stream_bytes=" << buf.size();
            buf.clear();
            conn->HandleClose();
            return;
        }
        break;  // Incomplete
    }
    // 同一读批次里心跳/重连先处理，避免排在 Move/EnterMap 后面拖过客户端时限。
    if (frames.size() > 1) {
        std::stable_sort(frames.begin(), frames.end(), [](const std::string &a, const std::string &b) {
            auto rank = [](const std::string &frame) {
                game::GameRequest peek;
                if (!peek.ParseFromString(frame))
                    return 2;
                if (peek.has_heartbeat() || peek.has_client_hello())
                    return 0;
                if (peek.has_reconnect() || peek.has_login() || peek.has_register_() ||
                    peek.has_logout())
                    return 1;
                return 2;
            };
            return rank(a) < rank(b);
        });
    }
    // 恶意半包堆积：超过读缓冲上限则关连
    if (buf.size() > conn->max_read_buf_bytes()) {
        LOG_WARN << "GameTcpGateway stream overflow conn#" << conn->id();
        buf.clear();
        conn->HandleClose();
        return;
    }

    for (auto &frame : frames) {
        auto tcp_sink = std::make_shared<TcpReplySink>(conn);
        GatewayConnRegistry::Bind sticky;
        const bool bound =
            GatewayConnRegistry::Instance().FindByConnection(conn->id(), &sticky);

        const std::string frame_lim =
            GatewayConnGuard::Instance().CheckFrameRate(conn->id(), frame.size());
        if (!frame_lim.empty()) {
            SendPublicErr(tcp_sink, conn->id(), 0, gameproto::kErrRateLimited, "frame rate limited");
            continue;
        }
        GatewayConnGuard::Instance().NoteActivity(conn->id(), frame.size());

        game::GameRequest peek;
        if (!peek.ParseFromString(frame)) {
            OpsMetrics::Instance().IncIllegalFrame();
            SendPublicErr(tcp_sink, conn->id(), 0, gameproto::kErrInvalidArgument, "invalid protobuf");
            LOG_WARN << "GameTcpGateway illegal protobuf conn#" << conn->id();
            conn->HandleClose();
            return;
        }

        const char *pkt_kind = "other";
        if (peek.has_heartbeat())
            pkt_kind = "heartbeat";
        else if (peek.has_reconnect())
            pkt_kind = "reconnect";
        else if (peek.has_login())
            pkt_kind = "login";
        else if (peek.has_register_())
            pkt_kind = "register";
        else if (peek.has_enter_map())
            pkt_kind = "enter_map";
        else if (peek.has_query_map_lines())
            pkt_kind = "query_lines";
        else if (peek.has_move())
            pkt_kind = "move";
        else if (peek.has_world_snapshot())
            pkt_kind = "snapshot";
        else if (peek.has_friend_list())
            pkt_kind = "friend";
        else if (peek.has_push_ack())
            pkt_kind = "push_ack";
        struct HandlerSpan {
            const char *kind = "other";
            uint64_t conn_id = 0;
            uint64_t player = 0;
            uint64_t seq = 0;
            TcpConnection *live = nullptr;
            bool sample = false;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~HandlerSpan() {
                if (!live)
                    return;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                int app = 0;
                int unsent = 0;
                if (ms >= 50 || sample) {
                    app = live->send_buf() ? live->send_buf()->readablebytes() : 0;
                    unsent = live->unsent_bytes();
                    NoteSendPending(app, unsent);
                }
                NotePktStage(kind, "handler", ms, conn_id, player, seq, app, unsent);
            }
        } handler_span;
        handler_span.kind = pkt_kind;
        handler_span.conn_id = conn->id();
        handler_span.player = bound ? sticky.player_id : 0;
        handler_span.seq = peek.seq();
        handler_span.live = conn.get();
        handler_span.sample = peek.has_heartbeat() || peek.has_reconnect() || peek.has_login() ||
                              peek.has_enter_map() || peek.has_query_map_lines() ||
                              peek.has_world_snapshot() || peek.has_friend_list();

        if (peek.has_client_hello()) {
            game::GameResponse outer;
            outer.set_seq(peek.seq());
            game::ServerHelloRsp hello;
            gameproto::HandleClientHello(peek.client_hello(), conn->id(), &hello, &outer);
            hello.set_gameplay_config_version(MapCatalog::Instance().gameplay_config_version());
            hello.set_map_manifest_version(MapCatalog::Instance().map_manifest_version());
            hello.clear_maps();
            for (const auto &ent : MapCatalog::Instance().ManifestEntries()) {
                auto *m = hello.add_maps();
                m->set_map_template_id(ent.map_template_id);
                m->set_data_version(ent.data_version);
                m->set_sha256(ent.sha256);
                m->set_scene_name(ent.scene_name);
                m->set_kind(SceneKindToString(ent.policy.kind));
                if (ent.visual_map_template_id != 0)
                    m->set_visual_map_template_id(ent.visual_map_template_id);
                for (const auto &p : ent.portals) {
                    auto *pp = m->add_portals();
                    pp->set_portal_id(p.portal_id);
                    pp->set_from_map_template_id(p.from_map_template_id);
                    pp->set_to_map_template_id(p.to_map_template_id);
                    pp->mutable_position()->set_x(p.position.x);
                    pp->mutable_position()->set_y(p.position.y);
                    pp->mutable_position()->set_z(p.position.z);
                    pp->set_yaw(p.yaw);
                    pp->set_trigger_radius(p.trigger_radius);
                }
            }
            *outer.mutable_server_hello() = hello;
            if (hello.ok()) {
                GatewayConnGuard::Instance().SetHelloOk(conn->id(), true);
                OpsMetrics::Instance().IncHelloOk();
                LOG_INFO << "ClientHello ok conn#" << conn->id()
                         << " platform=" << peek.client_hello().platform()
                         << " client_version=" << peek.client_hello().client_version()
                         << " maps=" << hello.maps_size()
                         << " manifest_ver=" << hello.map_manifest_version();
            } else {
                GatewayConnGuard::Instance().SetHelloOk(conn->id(), false);
                OpsMetrics::Instance().IncHelloFail();
                LOG_WARN << "ClientHello fail conn#" << conn->id()
                         << " code=" << hello.error_code();
            }
            std::string body, out;
            if (outer.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                tcp_sink->SendFrame(out);
            continue;
        }

        if (peek.has_heartbeat()) {
            if (ClientHelloRequired() && !GatewayConnGuard::Instance().HelloOk(conn->id())) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrUnauthenticated,
                              "hello required");
                continue;
            }
            const std::string hb_lim =
                GatewayConnGuard::Instance().CheckHeartbeatRate(conn->id(), bound);
            if (!hb_lim.empty()) {
                OpsMetrics::Instance().IncHeartbeatLimited();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "heartbeat rate limited");
                continue;
            }
            game::GameResponse outer;
            outer.set_seq(peek.seq());
            outer.set_ok(true);
            auto *hb = outer.mutable_heartbeat();
            hb->set_ok(true);
            hb->set_error_code(gameproto::kErrOk);
            hb->set_server_time_ms(gameproto::PublicNowMs());
            hb->set_echo_ms(peek.heartbeat().echo_ms());
            hb->set_server_recv_ms(gameproto::PublicNowMs());
            hb->set_jitter_hint_ms(gameproto::HeartbeatIntervalMs() / 5);
            gameproto::PromotePublicError(&outer, conn->id());
            std::string body, out;
            // 心跳在本连接 EventLoop 同步应答：TrySendUrgent，绝不 Hard Send/踢连接。
            if (outer.SerializeToString(&body) && gameproto::EncodeFrame(body, &out)) {
                MakeRoomForControl(conn.get());
                if (conn->TrySendControl(out, true)) {
                    OpsMetrics::Instance().AddGatewayTxBytes(out.size());
                    OpsMetrics::Instance().AddGatewayTxFrames(1);
                    OpsMetrics::Instance().IncHeartbeatOk();
                } else {
                    OpsMetrics::Instance().IncHeartbeatSoftDrop();
                    LOG_WARN << "heartbeat soft-drop conn#" << conn->id()
                             << " send_buf="
                             << (conn->send_buf() ? conn->send_buf()->readablebytes() : 0);
                }
            }
            continue;
        }

        if (ClientHelloRequired() && !GatewayConnGuard::Instance().HelloOk(conn->id()) &&
            (peek.has_login() || peek.has_register_() || peek.has_reconnect())) {
            SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrUnauthenticated,
                          "hello required");
            continue;
        }

#ifdef WEBSERVER_ENABLE_BRPC
        // 登录前白名单；未绑定业务命令 fail-closed
        if (!bound && !gameproto::IsPreAuthWhitelistPayload(frame) &&
            !gameproto::IsGatewayOwnedAuthPayload(frame)) {
            SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrUnauthenticated,
                          "unauthenticated");
            continue;
        }

        // Login / Logout / Reconnect：投递到 PlayerSerialQueue，避免阻塞 Reactor
        if (gameproto::IsGatewayOwnedAuthPayload(frame)) {
            game::GameRequest peek;
            peek.ParseFromString(frame);
            // 摘流：拒绝新 Login/Register；Reconnect/Logout 仍放行以便迁移
            if (ServiceHealth::Instance().draining() &&
                (peek.has_login() || peek.has_register_())) {
                OpsMetrics::Instance().IncDrainReject();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                              "gateway draining");
                continue;
            }
            const std::string auth_lim = GatewayConnGuard::Instance().CheckAuthCommandRate(conn->id());
            if (!auth_lim.empty() && (peek.has_login() || peek.has_register_())) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "auth rate limited");
                continue;
            }
            const uint64_t shard_key = bound                          ? sticky.player_id
                                       : peek.has_login()              ? peek.login().player_id()
                                       : peek.has_reconnect()          ? peek.reconnect().player_id()
                                       : peek.has_logout()             ? peek.logout().player_id()
                                                                       : conn->id();
            const uint64_t conn_id = conn->id();
            const std::string gw = g_gateway_id;
            auto sink = tcp_sink;
            std::string payload = frame;

            // Register/Logout 不占用 Login/Reconnect 流程槽；Login/Reconnect 同时仅一个
            uint64_t flow_gen = 0;
            const bool needs_flow = peek.has_login() || peek.has_reconnect();
            bool admission_held = false;
            if (needs_flow) {
                if (!LoginAdmission::Instance().TryAcquire(peek.has_reconnect())) {
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "login admission");
                    continue;
                }
                admission_held = true;
            }
            if (needs_flow && !GatewayAuthFlow::Instance().TryBegin(conn_id, &flow_gen)) {
                if (admission_held)
                    LoginAdmission::Instance().Release();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                              "auth flow in progress");
                continue;
            }

            auto finish = [sink, conn_id, flow_gen, needs_flow](
                              bool ok, std::string out, gameproto::GatewayLoginRoute route,
                              bool is_login, bool is_reconnect, bool is_register) {
                if (is_login) {
                    if (ok)
                        OpsMetrics::Instance().IncLoginOk();
                    else
                        OpsMetrics::Instance().IncLoginFail();
                    if (ok && route.player_id != 0) {
                        if (GatewayAuthFlow::Instance().AcceptCallback(conn_id, flow_gen)) {
                            RememberBind(conn_id, route.player_id, route.fence_token,
                                         route.session_id, route.generation, sink, &route);
                        } else {
                            gameproto::CompensateGatewaySession(route.player_id, route.session_id,
                                                                route.fence_token, route.generation);
                            LOG_WARN << "login bind skipped (stale flow/conn) player="
                                     << route.player_id << " conn=" << conn_id;
                            out.clear();
                        }
                    }
                } else if (is_register) {
                    if (ok)
                        OpsMetrics::Instance().IncRegisterOk();
                    else
                        OpsMetrics::Instance().IncRegisterFail();
                } else if (is_reconnect) {
                    if (ok)
                        OpsMetrics::Instance().IncReconnectOk();
                    else
                        OpsMetrics::Instance().IncReconnectFail();
                    if (ok && route.player_id != 0) {
                        if (GatewayAuthFlow::Instance().AcceptCallback(conn_id, flow_gen)) {
                            RememberBind(conn_id, route.player_id, route.fence_token,
                                         route.session_id, route.generation, sink, &route);
                            if (!out.empty() && sink &&
                                GatewayAuthFlow::Instance().Alive(conn_id)) {
                                sink->SendFrame(out);
                                out.clear();
                                for (const auto &pl : route.pending_push_payloads) {
                                    std::string frame;
                                    if (pl.size() >= 4) {
                                        const uint32_t be =
                                            (static_cast<uint8_t>(pl[0]) << 24) |
                                            (static_cast<uint8_t>(pl[1]) << 16) |
                                            (static_cast<uint8_t>(pl[2]) << 8) |
                                            static_cast<uint8_t>(pl[3]);
                                        if (be + 4 == pl.size())
                                            frame = pl;
                                    }
                                    if (frame.empty() && !gameproto::EncodeFrame(pl, &frame))
                                        continue;
                                    GatewayConnRegistry::Instance().SendBySession(route.session_id,
                                                                                frame);
                                }
                            }
                        } else {
                            gameproto::CompensateGatewaySession(route.player_id, route.session_id,
                                                                route.fence_token, route.generation);
                            LOG_WARN << "reconnect bind skipped (stale flow/conn) player="
                                     << route.player_id << " conn=" << conn_id;
                            out.clear();
                        }
                    }
                }
                if (needs_flow) {
                    GatewayAuthFlow::Instance().End(conn_id, flow_gen);
                    LoginAdmission::Instance().Release();
                }
                if (!out.empty() && sink &&
                    (!needs_flow || GatewayAuthFlow::Instance().Alive(conn_id))) {
                    sink->SendFrame(out);
                }
            };

            bool started = false;
            if (peek.has_login()) {
                started = gameproto::BeginOrchestrateGatewayLogin(
                    gw, conn_id, payload, shard_key,
                    [finish](bool ok, std::string out, gameproto::GatewayLoginRoute route) {
                        finish(ok, std::move(out), std::move(route), true, false, false);
                    });
            } else if (peek.has_register_()) {
                started = gameproto::BeginOrchestrateGatewayRegister(
                    payload, shard_key,
                    [finish](bool ok, std::string out, gameproto::GatewayLoginRoute route) {
                        finish(ok, std::move(out), std::move(route), false, false, true);
                    });
            } else if (peek.has_reconnect()) {
                started = gameproto::BeginOrchestrateGatewayReconnect(
                    gw, conn_id, payload, shard_key,
                    [finish](bool ok, std::string out, gameproto::GatewayLoginRoute route) {
                        finish(ok, std::move(out), std::move(route), false, true, false);
                    });
            } else {
                started = PlayerSerialQueue::Instance().TryPost(
                    shard_key, [gw, conn_id, payload, sink]() {
                        std::string out;
                        gameproto::GatewayLogoutResult lr;
                        if (HasAuthoritativeLogout(conn_id)) {
                            game::GameRequest req;
                            game::GameResponse rsp;
                            if (req.ParseFromString(payload))
                                rsp.set_seq(req.seq());
                            rsp.set_ok(true);
                            rsp.set_error_code("OK");
                            rsp.set_message("already offline");
                            auto *body = rsp.mutable_logout();
                            body->set_ok(true);
                            body->set_message("already offline");
                            std::string b;
                            if (rsp.SerializeToString(&b))
                                gameproto::EncodeFrame(b, &out);
                            lr.encoded = !out.empty();
                            lr.session_ok = true;
                            lr.logic_ok = true;
                        } else {
                            (void)gameproto::OrchestrateGatewayLogout(gw, conn_id, payload, &out,
                                                                      &lr);
                            if (lr.clear_bind)
                                MarkAuthoritativeLogout(conn_id);
                        }
                        if (!out.empty() && sink)
                            sink->SendFrame(out);
                    });
            }
            if (!started) {
                if (needs_flow) {
                    GatewayAuthFlow::Instance().End(conn_id, flow_gen);
                    LoginAdmission::Instance().Release();
                }
                OpsMetrics::Instance().IncQueueOverload();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                              "queue overloaded");
            }
            continue;
        }
#endif

        // 公网命令策略：Formal 封闭 GrantItem/MailDeliver；未登记 body 默认拒绝
        {
            std::string cerr;
            if (!gameproto::AllowClientTcpPayload(frame, bound, FormalModeEnabled(), &cerr)) {
                OpsMetrics::Instance().IncCommandForbidden();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrCommandForbidden,
                              cerr.empty() ? "command forbidden" : cerr.c_str());
                continue;
            }
        }

        if (peek.has_chat_send()) {
            const std::string chat_lim = GatewayConnGuard::Instance().CheckChatRate(conn->id());
            if (!chat_lim.empty()) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "chat rate limited");
                continue;
            }
        }
        if (peek.has_get_player_brief() && !peek.get_player_brief().player_name().empty()) {
            const std::string nlim = GatewayConnGuard::Instance().CheckNameQueryRate(conn->id());
            if (!nlim.empty()) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "name query rate limited");
                continue;
            }
        }

        SessionHandle handle;
        handle.connection_id = conn->id();
        if (bound) {
            // 覆盖客户端身份：不信任自报 player_id / route
            handle.player_id = sticky.player_id;
            handle.session_id = sticky.session_id;
            handle.fence_token = sticky.token;
            handle.generation = sticky.generation;
            handle.gamelogic_instance_id = sticky.gamelogic_instance_id;
            handle.map_instance_id = sticky.map_instance_id;
            handle.owner_epoch = sticky.map_owner_epoch;
            handle.route_version = sticky.route_version;
        } else {
            // 仅 Register 等白名单可到此（尚未绑定）
            handle.player_id = gameproto::ExtractPlayerIdFromRequestPayload(frame);
        }

#ifdef WEBSERVER_ENABLE_BRPC
        if (bound) {
            game::GameRequest peek;
            if (peek.ParseFromString(frame) && peek.has_push_ack()) {
                game::GameResponse rsp;
                rsp.set_seq(peek.seq());
                auto *ack = rsp.mutable_push_ack();
                const uint64_t pid = sticky.player_id != 0 ? sticky.player_id
                                                          : peek.push_ack().player_id();
                const uint64_t aseq = peek.push_ack().ack_server_seq();
                const std::string ack_sid = peek.push_ack().session_id().empty()
                                               ? sticky.session_id
                                               : peek.push_ack().session_id();
                const std::string ack_fence = peek.push_ack().fence_token().empty()
                                                 ? sticky.token
                                                 : peek.push_ack().fence_token();
                const uint64_t ack_gen = peek.push_ack().generation() != 0
                                            ? peek.push_ack().generation()
                                            : sticky.generation;
                bool ok_ack = false;
                std::string ack_msg = "ack rejected";
                uint64_t trimmed = 0;
                // 仅当前 session/fence/generation 可裁剪；旧 Session ACK 不得影响新 Session
                if (pid == sticky.player_id && aseq > 0 && ack_sid == sticky.session_id &&
                    (ack_fence.empty() || ack_fence == sticky.token) &&
                    (ack_gen == 0 || ack_gen == sticky.generation)) {
#ifdef WEBSERVER_ENABLE_REDIS
                    if (PushReplayStore::Instance().Available()) {
                        auto ar = PushReplayStore::Instance().Ack(pid, ack_sid, aseq);
                        ok_ack = ar.ok();
                        trimmed = ar.trimmed_to_seq;
                        if (ar.status == PushReplayStore::AckStatus::Ok) {
                            OpsMetrics::Instance().IncPushAckOk();
                            ack_msg = "acked";
                        } else if (ar.status == PushReplayStore::AckStatus::Duplicate) {
                            OpsMetrics::Instance().IncPushAckDuplicate();
                            ack_msg = "acked";
                        } else if (ar.status == PushReplayStore::AckStatus::Ahead) {
                            OpsMetrics::Instance().IncPushAckAheadRejected();
                            ack_msg = ar.error_code.empty() ? "ERR_ACK_AHEAD" : ar.error_code;
                        } else if (ar.status == PushReplayStore::AckStatus::Stale) {
                            OpsMetrics::Instance().IncPushAckStaleRejected();
                            ack_msg = gameproto::kErrAoiResyncRequired;
                        } else if (ar.status == PushReplayStore::AckStatus::Gap) {
                            ack_msg = gameproto::kErrAoiResyncRequired;
                        } else {
                            ack_msg = ar.error_code.empty() ? "ack rejected" : ar.error_code;
                        }
                    } else {
                        ack_msg = "ERR_ACK_UNAVAILABLE";
                    }
#else
                    ack_msg = "ERR_ACK_UNAVAILABLE";
#endif
                } else {
                    OpsMetrics::Instance().IncPushAckStaleRejected();
                    ack_msg = "ERR_ACK_STALE";
                }
                ack->set_ok(ok_ack);
                ack->set_message(ack_msg);
                ack->set_trimmed_to_seq(ok_ack ? trimmed : 0);
                rsp.set_ok(ok_ack);
                rsp.set_message(ack->message());
                gameproto::PromotePublicError(&rsp, conn->id());
                std::string body, out;
                if (rsp.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                    tcp_sink->SendFrame(out);
                continue;
            }
            // EnterMap：在 PlayerSerialQueue 编排 Transfer，禁止 Reactor 同步等 brpc
            if (peek.has_enqueue_map()) {
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                if (!gameproto::BeginOrchestrateGatewayEnqueueMap(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle) {
                            (void)ok;
                            (void)conn_id;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_switch_line()) {
                if (ServiceHealth::Instance().draining()) {
                    OpsMetrics::Instance().IncDrainReject();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "gateway draining");
                    continue;
                }
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                if (!gameproto::BeginOrchestrateGatewaySwitchLine(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle route) {
                            if (ok) {
                                GatewayConnRegistry::Instance().ApplyRoute(
                                    conn_id, route.gamelogic_instance_id, route.map_instance_id,
                                    route.owner_epoch, route.route_version);
                            }
                            (void)ok;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_create_dungeon()) {
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                if (!gameproto::BeginOrchestrateGatewayCreateDungeon(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle) {
                            (void)ok;
                            (void)conn_id;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_interact_portal()) {
                if (ServiceHealth::Instance().draining()) {
                    OpsMetrics::Instance().IncDrainReject();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "gateway draining");
                    continue;
                }
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                if (!gameproto::BeginOrchestrateGatewayInteractPortal(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle route) {
                            if (ok) {
                                GatewayConnRegistry::Instance().ApplyRoute(
                                    conn_id, route.gamelogic_instance_id, route.map_instance_id,
                                    route.owner_epoch, route.route_version);
                            }
                            (void)ok;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_query_map_lines()) {
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                if (!gameproto::BeginOrchestrateGatewayQueryMapLines(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle) {
                            (void)ok;
                            (void)conn_id;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_enter_map()) {
                if (ServiceHealth::Instance().draining()) {
                    OpsMetrics::Instance().IncDrainReject();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "gateway draining");
                    continue;
                }
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                if (!gameproto::BeginOrchestrateGatewayEnterMap(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle route) {
                            if (ok) {
                                GatewayConnRegistry::Instance().ApplyRoute(
                                    conn_id, route.gamelogic_instance_id, route.map_instance_id,
                                    route.owner_epoch, route.route_version);
                            }
                            (void)ok;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
        }
#endif

        GameRequestTransport::Get().PostPlayerRequest(handle, std::move(frame),
                                                     std::move(tcp_sink));
    }
}
