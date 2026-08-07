/**
 * @file GameTcpGateway.cpp
 * @brief 网关：拆帧投递 + Login/Reconnect 本地绑定 + 断线 MarkDisconnected
 */

#include "GameTcpGateway.h"

#include "Buffer.h"
#include "EventLoop.h"
#include "GameRequestPlayerId.h"
#include "GameRequestTransport.h"
#include "GatewayConnRegistry.h"
#include "InProcessTransport.h"
#include "Logging.h"
#include "ProtoFraming.h"
#include "ReplySink.h"
#include "SessionHandle.h"
#include "TcpConnection.h"
#include "TcpReplySink.h"
#include "TcpServer.h"
#include "game.pb.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#ifdef WEBSERVER_ENABLE_BRPC
#include "GatewayLoginOrchestrator.h"
#include "SessionRpcClient.h"
#endif
#endif

#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::mutex g_stream_mu;
std::unordered_map<int, std::string> g_stream_buf;

struct ConnBind {
    uint64_t player_id = 0;
    std::string token;
    std::string session_id;
    uint64_t generation = 0;
};

std::mutex g_bind_mu;
std::unordered_map<int, ConnBind> g_conn_bind;  // connection_id -> bind
std::string g_gateway_id = "gw:local";

std::string &StreamBuf(int conn_id) {
    return g_stream_buf[conn_id];
}

void RememberBind(int conn_id, uint64_t player_id, const std::string &token,
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

void ForgetBind(int conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_conn_bind.erase(conn_id);
    GatewayConnRegistry::Instance().Forget(conn_id);
}

class BindingReplySink : public ReplySink {
public:
    BindingReplySink(std::shared_ptr<ReplySink> inner, int conn_id, uint64_t player_id,
                     bool is_login_or_reconnect)
        : inner_(std::move(inner)),
          conn_id_(conn_id),
          player_id_(player_id),
          bindable_(is_login_or_reconnect) {}

    void SendFrame(const std::string &response_frame) override {
        if (bindable_ && player_id_ != 0) {
            std::string buf = response_frame;
            std::string payload;
            if (gameproto::TryDecodeOneFrame(&buf, &payload)) {
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
    int conn_id_ = 0;
    uint64_t player_id_ = 0;
    bool bindable_ = false;
};

bool IsLoginOrReconnectPayload(const std::string &payload) {
    game::GameRequest req;
    if (!req.ParseFromString(payload))
        return false;
    return req.body_case() == game::GameRequest::kLogin ||
           req.body_case() == game::GameRequest::kReconnect;
}

}  // namespace

GameTcpGateway::GameTcpGateway(const std::string &ip, int port) : ip_(ip), port_(port) {
    std::ostringstream os;
    os << "gw:" << ip << ":" << port;
    g_gateway_id = os.str();
}

GameTcpGateway::~GameTcpGateway() {
    if (thread_.joinable())
        thread_.join();
}

void GameTcpGateway::StartInBackground() {
    thread_ = std::thread([this]() { Run(); });
}

void GameTcpGateway::Run() {
    InProcessTransport::Instance().EnsureStarted(0);

    EventLoop loop;
    auto server = std::make_unique<TcpServer>(&loop, ip_.c_str(), port_);
    int workers = static_cast<int>(std::thread::hardware_concurrency());
    if (workers <= 1)
        workers = 1;
    else
        --workers;
    server->SetThreadNums(workers);
    server->set_message_callback([this](const std::shared_ptr<TcpConnection> &c) { OnMessage(c); });
    server->set_disconnect_callback([](const std::shared_ptr<TcpConnection> &c) {
        ConnBind bind;
        {
            std::lock_guard<std::mutex> lk(g_bind_mu);
            auto it = g_conn_bind.find(c->id());
            if (it == g_conn_bind.end())
                return;
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
}

void GameTcpGateway::OnMessage(const std::shared_ptr<TcpConnection> &conn) {
    Buffer *rb = conn->read_buf();
    if (!rb || rb->readablebytes() <= 0)
        return;

    std::vector<std::string> frames;
    {
        std::lock_guard<std::mutex> lk(g_stream_mu);
        std::string &buf = StreamBuf(conn->id());
        buf.append(rb->RetrieveAllAsString());

        std::string frame;
        while (gameproto::TryDecodeOneFrame(&buf, &frame))
            frames.push_back(std::move(frame));
    }

    for (auto &frame : frames) {
        const uint64_t player_id = gameproto::ExtractPlayerIdFromRequestPayload(frame);
        SessionHandle handle;
        handle.player_id = player_id;
        handle.connection_id = conn->id();
        auto tcp_sink = std::make_shared<TcpReplySink>(conn);

#ifdef WEBSERVER_ENABLE_BRPC
        // Login/Logout：Gateway 编排 Auth+Session+BindPlayer，不再转发 GameLogic Login RPC
        if (gameproto::IsGatewayOwnedAuthPayload(frame)) {
            std::string out;
            game::GameRequest peek;
            const bool is_login = peek.ParseFromString(frame) && peek.has_login();
            bool ok = false;
            gameproto::GatewayLoginRoute route;
            if (is_login)
                ok = gameproto::OrchestrateGatewayLogin(g_gateway_id, conn->id(), frame, &out,
                                                        &route);
            else
                ok = gameproto::OrchestrateGatewayLogout(g_gateway_id, conn->id(), frame, &out);
            if (!out.empty()) {
                if (is_login && ok && route.player_id != 0) {
                    RememberBind(conn->id(), route.player_id, route.fence_token, route.session_id,
                                 route.generation, tcp_sink, &route);
                }
                tcp_sink->SendFrame(out);
            }
            continue;
        }
#endif

        // 粘性路由：用 Session 返回的 gamelogic_instance_id，不信任客户端自报
        GatewayConnRegistry::Bind sticky;
        if (GatewayConnRegistry::Instance().FindByConnection(conn->id(), &sticky)) {
            handle.session_id = sticky.session_id;
            handle.fence_token = sticky.token;
            handle.generation = sticky.generation;
            handle.gamelogic_instance_id = sticky.gamelogic_instance_id;
            handle.map_instance_id = sticky.map_instance_id;
            handle.owner_epoch = sticky.map_owner_epoch;
            handle.route_version = sticky.route_version;
        }

        const bool bindable = IsLoginOrReconnectPayload(frame);
        std::shared_ptr<ReplySink> sink = tcp_sink;
        if (bindable) {
            sink = std::make_shared<BindingReplySink>(tcp_sink, conn->id(), player_id, true);
        }
        GameRequestTransport::Get().PostPlayerRequest(handle, std::move(frame), std::move(sink));
    }
}
