#include "GatewayPushServer.h"

#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "ProtoFraming.h"
#include "PublicError.h"
#include "PushAdmit.h"
#include "PushReplayCache.h"
#include "ReliableOutbox.h"
#include "game.pb.h"
#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#endif
#include <brpc/controller.h>
#include <brpc/server.h>
#include <bthread/bthread.h>

#include <cstdlib>

namespace {

uint64_t SeqAfterSessionMatch(const ::gwpush::PushMessage &m) {
    if (!m.reliable() || m.server_seq() != 0)
        return m.server_seq();
#ifdef WEBSERVER_ENABLE_REDIS
    if (PushReplayStore::Instance().Available()) {
        return PushReplayStore::Instance().AppendReliable(m.player_id(), m.session_id(),
                                                          m.message_type(), m.payload());
    }
#endif
    const uint64_t seq = PushReplayCache::Instance().NextSeq(m.player_id());
    if (seq == 0)
        return 0;
    PushReplayEntry entry;
    entry.server_seq = seq;
    entry.message_type = m.message_type();
    entry.payload = m.payload();
    entry.reliable = true;
    PushReplayCache::Instance().Store(m.player_id(), entry);
    return seq;
}

}  // namespace

void GatewayPushServiceImpl::PushBatch(::google::protobuf::RpcController *controller,
                                       const ::gwpush::PushBatchRequest *request,
                                       ::gwpush::PushBatchResponse *response,
                                       ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request) {
        response->set_ok(false);
        response->set_message("null request");
        return;
    }
    const std::string &local_gw = GatewayPushServer::Instance().gateway_instance_id();
    if (!local_gw.empty() && !request->gateway_instance_id().empty() &&
        request->gateway_instance_id() != local_gw) {
        for (const auto &m : request->messages()) {
            auto *row = response->add_results();
            row->set_player_id(m.player_id());
            row->set_reason(gwpush::GATEWAY_MISMATCH);
            OpsMetrics::Instance().IncReliablePushReject(
                static_cast<int>(PushAdmitReason::GatewayMismatch));
        }
        response->set_ok(false);
        response->set_rejected(static_cast<uint32_t>(request->messages_size()));
        response->set_message("gateway_instance_id mismatch");
        return;
    }
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    for (const auto &m : request->messages()) {
        GatewayConnRegistry::Bind bind;
        const bool found = GatewayConnRegistry::Instance().FindBySession(m.session_id(), &bind);
        PushBindView view;
        view.found = found;
        if (found) {
            view.player_id = bind.player_id;
            view.gateway_instance_id = bind.gateway_instance_id;
            view.token = bind.token;
            view.generation = bind.generation;
            view.has_sender = static_cast<bool>(m.reliable() ? bind.send_frame_urgent : bind.send_frame);
            if (!view.has_sender)
                view.has_sender = static_cast<bool>(bind.send_frame);
        }
        PushMsgView msg;
        msg.player_id = m.player_id();
        msg.fence_token = m.fence_token();
        msg.generation = m.generation();
        msg.reliable = m.reliable();
        msg.server_seq = m.server_seq();
        msg.payload = m.payload();
        const std::string payload_copy = m.payload();
        const std::string message_type = m.message_type();
        const bool reliable = m.reliable();
        const bool coalescable = m.coalescable();
        uint64_t assigned_seq = m.server_seq();
        ReliableOutboxHub::Order order;
        if (reliable && found)
            order = ReliableOutboxHub::Instance().Lock(bind.connection_id);
        PushAdmitEnv env;
        env.local_gateway_id = local_gw;
        env.assign_seq = [&m, &assigned_seq]() {
            assigned_seq = SeqAfterSessionMatch(m);
            return assigned_seq;
        };
        env.encode = [&](uint64_t seq, std::string *frame) {
            if (!frame)
                return false;
            frame->clear();
            const std::string &raw = payload_copy;
            const uint32_t be = (static_cast<uint8_t>(raw[0]) << 24) |
                                (static_cast<uint8_t>(raw[1]) << 16) |
                                (static_cast<uint8_t>(raw[2]) << 8) |
                                static_cast<uint8_t>(raw[3]);
            std::string inner_body;
            if (be + 4 == raw.size())
                inner_body = raw.substr(4);
            else
                inner_body = raw;
            if (seq != 0) {
                game::GameResponse env_msg;
                env_msg.set_ok(true);
                env_msg.set_message("server_push");
                auto *p = env_msg.mutable_server_push();
                p->set_server_seq(seq);
                p->set_message_type(message_type);
                p->set_payload(inner_body);
                p->set_reliable(reliable);
                p->set_coalescable(coalescable);
                std::string body;
                return env_msg.SerializeToString(&body) && gameproto::EncodeFrame(body, frame);
            }
            if (be + 4 == raw.size()) {
                *frame = raw;
                return true;
            }
            return gameproto::EncodeFrame(inner_body, frame);
        };
        env.send = [&](const std::string &frame) {
            if (reliable && found && order.q)
                return ReliableOutboxHub::Instance().Accept(order, assigned_seq, frame);
            return GatewayConnRegistry::Instance().SendBySession(m.session_id(), frame, false);
        };
        const PushAdmitOut decision = AdmitPushItem(msg, view, env);
        auto *row = response->add_results();
        row->set_player_id(m.player_id());
        row->set_server_seq(decision.server_seq);
        row->set_queued(decision.queued);
        row->set_replay_pending(decision.replay_pending);
        row->set_reason(static_cast<gwpush::PushRejectReason>(static_cast<int>(decision.reason)));
        if (decision.queued) {
            ++accepted;
            continue;
        }
        ++rejected;
        OpsMetrics::Instance().IncReliablePushReject(static_cast<int>(decision.reason));
    }
    response->set_ok(true);
    response->set_accepted(accepted);
    response->set_rejected(rejected);
    response->set_message("ok");
}

void GatewayPushServiceImpl::KickConnection(::google::protobuf::RpcController *controller,
                                            const ::gwpush::KickConnectionRequest *request,
                                            ::gwpush::KickConnectionResponse *response,
                                            ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_message("invalid kick");
        return;
    }
    const char *reason = request->reason().empty() ? "SESSION_REPLACED" : request->reason().c_str();
    game::GameResponse notify;
    notify.set_ok(true);
    notify.set_seq(0);
    notify.set_message("session replaced");
    notify.set_error_code("OK");
    notify.set_server_time_ms(gameproto::PublicNowMs());
    auto *body = notify.mutable_session_replaced();
    body->set_reason_code(reason);
    body->set_server_time_ms(notify.server_time_ms());
    body->set_message("logged in elsewhere");
    std::string payload;
    std::string frame;
    if (!notify.SerializeToString(&payload) || !gameproto::EncodeFrame(payload, &frame))
        frame.clear();
    double grace = 3.0;
    if (const char *e = std::getenv("GAMEMESH_SESSION_REPLACE_GRACE_SEC")) {
        char *end = nullptr;
        const double sec = std::strtod(e, &end);
        if (end != e && sec >= 0.0 && sec <= 30.0)
            grace = sec;
    } else if (const char *ms_env = std::getenv("GAMEMESH_KICK_NOTIFY_GRACE_MS")) {
        char *end = nullptr;
        const unsigned long ms = std::strtoul(ms_env, &end, 10);
        if (end != ms_env && ms <= 30000)
            grace = static_cast<double>(ms) / 1000.0;
    }
    const bool closed = GatewayConnRegistry::Instance().NotifyAndCloseIfMatch(
        request->player_id(), request->session_id(), request->generation(), frame, grace);
    response->set_ok(true);
    response->set_closed(closed);
    response->set_message(closed ? "closed" : "not_found");
    LOG_INFO << "KickConnection player=" << request->player_id()
             << " gen=" << request->generation() << " closed=" << closed
             << " reason=" << reason;
}

GatewayPushServer &GatewayPushServer::Instance() {
    static GatewayPushServer g;
    return g;
}

GatewayPushServer::~GatewayPushServer() { Stop(); }

bool GatewayPushServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;
    server_.reset(new brpc::Server());
    service_.reset(new GatewayPushServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GatewayPushServer AddService failed";
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "GatewayPushServer Start failed " << listen_addr;
        return false;
    }
    listen_addr_ = listen_addr;
    running_ = true;
    OpsMetrics::Instance().SetBrpcWorkerThreads(bthread_getconcurrency());
    LOG_INFO << "GatewayPushServer listening on " << listen_addr_
             << " gw=" << gateway_instance_id_;
    return true;
}

void GatewayPushServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    running_ = false;
}
