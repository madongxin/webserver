/**
 * @file GameTcpGateway.cpp
 * @brief 网关：拆帧投递 + Login/Reconnect 本地绑定 + 断线 MarkDisconnected
 */

#include "GameTcpGateway.h"

#include "Buffer.h"
#include "EventLoop.h"
#include "GameRequestPlayerId.h"
#include "GameRequestTransport.h"
#include "GatewayAuthFlow.h"
#include "GatewayAuthPolicy.h"
#include "GatewayConnRegistry.h"
#include "GatewayIdentity.h"
#include "InProcessTransport.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PlayerSerialQueue.h"
#include "ProtoFraming.h"
#include "ReplySink.h"
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
#include "GatewayEnterMapOrchestrator.h"
#include "GatewayLoginOrchestrator.h"
#include "SessionRpcClient.h"
#endif
#endif

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
        auto weak = std::weak_ptr<ReplySink>(sink);
        gb.send_frame = [weak](const std::string &frame) {
            if (auto s = weak.lock())
                s->SendFrame(frame);
        };
    }
    GatewayConnRegistry::Instance().Remember(conn_id, std::move(gb));
}

void ForgetBind(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_conn_bind.erase(conn_id);
    GatewayConnRegistry::Instance().Forget(conn_id);
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
        GatewayAuthFlow::Instance().OnConnected(c->id());
    });
    server->set_message_callback([this](const std::shared_ptr<TcpConnection> &c) { OnMessage(c); });
    server->set_disconnect_callback([](const std::shared_ptr<TcpConnection> &c) {
        OpsMetrics::Instance().IncTcpDisconnect();
        GatewayAuthFlow::Instance().OnDisconnected(c->id());
        ConnBind bind;
        {
            std::lock_guard<std::mutex> lk(g_bind_mu);
            auto it = g_conn_bind.find(c->id());
            if (it == g_conn_bind.end()) {
                ForgetBind(c->id());
                return;
            }
            bind = it->second;
            g_conn_bind.erase(it);
        }
#ifdef WEBSERVER_ENABLE_REDIS
        if (bind.player_id != 0) {
#ifdef WEBSERVER_ENABLE_BRPC
            if (SessionRpcClient::Instance().ready()) {
                SessionRpcClient::Instance().MarkDisconnected(bind.player_id, bind.token,
                                                             bind.generation);
            } else
#endif
                if (SessionStore::Instance().Available()) {
                SessionStore::Instance().MarkDisconnected(bind.player_id, bind.token,
                                                         bind.generation);
            }
        }
#endif
        if (bind.player_id != 0) {
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
        ForgetBind(c->id());
    });
    LOG_INFO << "GameTcpGateway ready on " << ip_ << ":" << port_
             << " id=" << g_gateway_id << " (session bind + grace disconnect)";
    server->Start();
    server_.store(nullptr);
    loop_.store(nullptr);
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

#ifdef WEBSERVER_ENABLE_BRPC
        // 登录前白名单；未绑定业务命令 fail-closed
        if (!bound && !gameproto::IsPreAuthWhitelistPayload(frame) &&
            !gameproto::IsGatewayOwnedAuthPayload(frame)) {
            game::GameResponse rsp;
            rsp.set_ok(false);
            rsp.set_message("unauthenticated");
            std::string body, out;
            if (rsp.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                tcp_sink->SendFrame(out);
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
                game::GameResponse dr;
                dr.set_ok(false);
                dr.set_message("gateway draining");
                std::string body, out;
                if (dr.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                    tcp_sink->SendFrame(out);
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
                game::GameResponse busy;
                busy.set_ok(false);
                busy.set_message("auth flow in progress");
                std::string body, out;
                if (busy.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                    tcp_sink->SendFrame(out);
                continue;
            }

            PlayerSerialQueue::Instance().Post(
                shard_key, [gw, conn_id, payload, sink, flow_gen, needs_flow]() {
                std::string out;
                gameproto::GatewayLoginRoute route;
                game::GameRequest req;
                req.ParseFromString(payload);
                bool ok = false;
                if (req.has_login()) {
                    ok = gameproto::OrchestrateGatewayLogin(gw, conn_id, payload, &out, &route);
                    if (ok)
                        OpsMetrics::Instance().IncLoginOk();
                    else
                        OpsMetrics::Instance().IncLoginFail();
                    if (ok && route.player_id != 0) {
                        if (GatewayAuthFlow::Instance().AcceptCallback(conn_id, flow_gen)) {
                            RememberBind(conn_id, route.player_id, route.fence_token,
                                         route.session_id, route.generation, sink, &route);
                        } else {
                            // 迟到回调或连接已断：幂等补偿，避免残留 ONLINE
                            gameproto::CompensateGatewaySession(route.player_id, route.session_id,
                                                                route.fence_token);
                            LOG_WARN << "login bind skipped (stale flow/conn) player="
                                     << route.player_id << " conn=" << conn_id;
                            out.clear();
                        }
                    }
                } else if (req.has_register_()) {
                    ok = gameproto::OrchestrateGatewayRegister(payload, &out);
                    if (ok)
                        OpsMetrics::Instance().IncRegisterOk();
                    else
                        OpsMetrics::Instance().IncRegisterFail();
                } else if (req.has_reconnect()) {
                    ok = gameproto::OrchestrateGatewayReconnect(gw, conn_id, payload, &out, &route);
                    if (ok)
                        OpsMetrics::Instance().IncReconnectOk();
                    else
                        OpsMetrics::Instance().IncReconnectFail();
                    if (ok && route.player_id != 0) {
                        if (GatewayAuthFlow::Instance().AcceptCallback(conn_id, flow_gen)) {
                            RememberBind(conn_id, route.player_id, route.fence_token,
                                         route.session_id, route.generation, sink, &route);
                            // ReconnectRsp → Replay 或 FullSnapshot（need_full_snapshot 时也必须发送）
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
                                                                route.fence_token);
                            LOG_WARN << "reconnect bind skipped (stale flow/conn) player="
                                     << route.player_id << " conn=" << conn_id;
                            out.clear();
                        }
                    }
                } else {
                    ok = gameproto::OrchestrateGatewayLogout(gw, conn_id, payload, &out);
                    if (ok)
                        ForgetBind(conn_id);
                }
                if (needs_flow)
                    GatewayAuthFlow::Instance().End(conn_id, flow_gen);
                (void)ok;
                // 连接已断则不再回写客户端
                if (!out.empty() && sink &&
                    (!needs_flow || GatewayAuthFlow::Instance().Alive(conn_id))) {
                    sink->SendFrame(out);
                }
            });
            continue;
        }
#endif

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
                // 仅当前 session/fence/generation 可裁剪；旧 Session ACK 不得影响新 Session
                if (pid == sticky.player_id && aseq > 0 && ack_sid == sticky.session_id &&
                    (ack_fence.empty() || ack_fence == sticky.token) &&
                    (ack_gen == 0 || ack_gen == sticky.generation)) {
#ifdef WEBSERVER_ENABLE_REDIS
                    if (PushReplayStore::Instance().Available())
                        ok_ack = PushReplayStore::Instance().Ack(pid, ack_sid, aseq);
#endif
                }
                ack->set_ok(ok_ack);
                ack->set_message(ok_ack ? "acked" : "ack rejected");
                ack->set_trimmed_to_seq(ok_ack ? aseq : 0);
                rsp.set_ok(ok_ack);
                rsp.set_message(ack->message());
                std::string body, out;
                if (rsp.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                    tcp_sink->SendFrame(out);
                continue;
            }
            // EnterMap：在 PlayerSerialQueue 编排 Transfer，禁止 Reactor 同步等 brpc
            if (peek.has_enter_map()) {
                if (ServiceHealth::Instance().draining()) {
                    OpsMetrics::Instance().IncDrainReject();
                    game::GameResponse dr;
                    dr.set_ok(false);
                    dr.set_seq(peek.seq());
                    dr.set_message("gateway draining");
                    std::string body, out;
                    if (dr.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                        tcp_sink->SendFrame(out);
                    continue;
                }
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                PlayerSerialQueue::Instance().Post(h.player_id, [h, conn_id, payload, sink]() {
                    std::string out;
                    SessionHandle route = h;
                    const bool ok =
                        gameproto::OrchestrateGatewayEnterMap(h, payload, &out, &route);
                    if (ok || !out.empty()) {
                        GatewayConnRegistry::Instance().ApplyRoute(
                            conn_id, route.gamelogic_instance_id, route.map_instance_id,
                            route.owner_epoch, route.route_version);
                        // 同步 g_conn_bind generation 不变，仅 registry 路由
                    }
                    (void)ok;
                    if (!out.empty() && sink)
                        sink->SendFrame(out);
                });
                continue;
            }
        }
#endif

        GameRequestTransport::Get().PostPlayerRequest(handle, std::move(frame),
                                                     std::move(tcp_sink));
    }
}
