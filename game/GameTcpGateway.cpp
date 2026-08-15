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
#include "OpsMetrics.h"
#include "PlayerSerialQueue.h"
#include "ProtocolHandshake.h"
#include "ProtoFraming.h"
#include "PublicError.h"
#include "ReplySink.h"
#include "RpcOffloadPool.h"
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
#include "SessionRpcClient.h"
#endif
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
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
        gb.send_frame = [weak_conn](const std::string &frame) {
            auto c = weak_conn.lock();
            if (!c)
                return;
            TcpReplySink live(c);
            live.SendFrame(frame);
        };
        gb.close_conn = [weak_conn]() {
            auto c = weak_conn.lock();
            if (!c)
                return;
            TcpReplySink live(c);
            live.CloseConnection();
        };
    }
    GatewayConnRegistry::Instance().Remember(conn_id, std::move(gb));
}

void ForgetBind(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_conn_bind.erase(conn_id);
    GatewayConnRegistry::Instance().Forget(conn_id);
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
        const bool has_reg = GatewayConnRegistry::Instance().FindByConnection(c->id(), &reg);
#endif
        // 必须在 g_bind_mu 之外 Forget：ForgetBind 会再锁同一把非递归 mutex。
        // 未绑定连接（Register 超时后客户端断开）走旧 early-return 会把 acceptor EventLoop 卡死，
        // listen backlog 堆满后所有新登录 no_response。
        ForgetBind(c->id());

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
                GameRequestTransport::Get().PostPlayerRequest(h, std::move(payload),
                                                             std::make_shared<DropSink>());
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

    // 流缓冲挂在连接上，仅由所属 EventLoop 访问，无需全局锁
    std::string &buf = conn->proto_stream();
    buf.append(rb->RetrieveAllAsString());

    std::vector<std::string> frames;
    std::string frame;
    for (;;) {
        const auto dr = gameproto::DecodeOneFrame(&buf, &frame);
        if (dr == gameproto::FrameDecodeResult::Complete) {
            frames.push_back(std::move(frame));
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

        if (peek.has_client_hello()) {
            game::GameResponse outer;
            outer.set_seq(peek.seq());
            game::ServerHelloRsp hello;
            gameproto::HandleClientHello(peek.client_hello(), conn->id(), &hello, &outer);
            if (hello.ok()) {
                GatewayConnGuard::Instance().SetHelloOk(conn->id(), true);
                OpsMetrics::Instance().IncHelloOk();
                LOG_INFO << "ClientHello ok conn#" << conn->id()
                         << " platform=" << peek.client_hello().platform()
                         << " client_version=" << peek.client_hello().client_version();
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
            if (outer.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                tcp_sink->SendFrame(out);
            OpsMetrics::Instance().IncHeartbeatOk();
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
            if (needs_flow && !GatewayAuthFlow::Instance().TryBegin(conn_id, &flow_gen)) {
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
                if (needs_flow)
                    GatewayAuthFlow::Instance().End(conn_id, flow_gen);
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
                        const bool ok =
                            gameproto::OrchestrateGatewayLogout(gw, conn_id, payload, &out);
                        if (ok)
                            ForgetBind(conn_id);
                        if (!out.empty() && sink)
                            sink->SendFrame(out);
                    });
            }
            if (!started) {
                if (needs_flow)
                    GatewayAuthFlow::Instance().End(conn_id, flow_gen);
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
                            if (ok || !out.empty()) {
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
