#include "GatewayAuthClients.h"

#include "BrpcNamingUtil.h"
#include "BrpcSslUtil.h"
#include "GatewayConfigPath.h"
#include "Logging.h"

#include <brpc/channel.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <unistd.h>

GatewayAuthClients &GatewayAuthClients::Instance() {
    static GatewayAuthClients g;
    return g;
}

void GatewayAuthClients::Publish(std::shared_ptr<RpcChannelSnapshot> next) {
    if (!next)
        return;
    next->version = version_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::atomic_store_explicit(&snap_, std::shared_ptr<const RpcChannelSnapshot>(std::move(next)),
                               std::memory_order_release);
}

std::shared_ptr<const RpcChannelSnapshot> GatewayAuthClients::CurrentSnapshot() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
}

bool GatewayAuthClients::ready() const {
    auto s = CurrentSnapshot();
    return s && s->session != nullptr;
}

size_t GatewayAuthClients::session_peer_count() const {
    auto s = CurrentSnapshot();
    return s ? s->session_peer_count : 0;
}

bool GatewayAuthClients::InitAuthSession(const std::string &session_addr_or_csv, int timeout_ms) {
    std::vector<std::string> addrs;
    if (session_addr_or_csv.rfind("list://", 0) == 0) {
        addrs.push_back(session_addr_or_csv);
    } else {
        GatewayConfigPath::SplitCsv(session_addr_or_csv, &addrs);
    }
    return InitAuthSession(addrs, timeout_ms);
}

bool GatewayAuthClients::InitAuthSession(const std::vector<std::string> &session_addrs,
                                         int timeout_ms) {
    if (session_addrs.empty()) {
        LOG_WARN << "GatewayAuthClients: ignore empty InitAuthSession (keep last snapshot)";
        return ready();
    }
    timeout_ms_ = timeout_ms;
    const std::string naming = BuildListNamingUrl(session_addrs);
    auto ch = std::make_shared<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.timeout_ms = timeout_ms_;
    opt.max_retry = 0;  // Acquire/Reconnect 等变更禁止自动重试
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::ApplyChannel(&opt, ssl);
    const std::string lb = SessionLoadBalancerName(session_addrs.size());
    const int rc = lb.empty() ? ch->Init(naming.c_str(), &opt)
                              : ch->Init(naming.c_str(), lb.c_str(), &opt);
    if (rc != 0) {
        LOG_ERROR << "GatewayAuthClients session Init failed naming=" << naming
                  << " (keep last snapshot)";
        return false;
    }

    auto prev = CurrentSnapshot();
    auto next = std::make_shared<RpcChannelSnapshot>();
    next->session = std::move(ch);
    next->session_peer_count = session_addrs.size();
    if (prev) {
        next->logic = prev->logic;
        next->logic_addrs = prev->logic_addrs;
        next->logic_ids = prev->logic_ids;
    }
    Publish(std::move(next));
    LOG_INFO << "GatewayAuthClients session ready naming=" << naming
             << " peers=" << session_addrs.size() << " lb=" << (lb.empty() ? "none" : lb);
    return true;
}

bool GatewayAuthClients::InitLogicChannels(const std::vector<std::string> &logic_addrs,
                                           const std::vector<std::string> &logic_ids,
                                           int timeout_ms) {
    if (logic_addrs.empty()) {
        LOG_WARN << "GatewayAuthClients: ignore empty InitLogicChannels (keep last snapshot)";
        auto s = CurrentSnapshot();
        return s && !s->logic.empty();
    }
    timeout_ms_ = timeout_ms;
    std::unordered_map<std::string, std::shared_ptr<brpc::Channel>> built;
    std::vector<std::string> ids;
    ids.reserve(logic_addrs.size());
    auto prev = CurrentSnapshot();
    for (size_t i = 0; i < logic_addrs.size(); ++i) {
        const std::string id =
            i < logic_ids.size() && !logic_ids[i].empty() ? logic_ids[i]
                                                         : ("gl-" + std::to_string(i));
        ids.push_back(id);
        if (prev) {
            auto it = prev->logic.find(id);
            if (it != prev->logic.end() && it->second && i < prev->logic_addrs.size() &&
                prev->logic_addrs[i] == logic_addrs[i] &&
                i < prev->logic_ids.size() && prev->logic_ids[i] == id) {
                built[id] = it->second;
                continue;
            }
            // 同 id 不同 addr：重建；同 addr 可复用
            if (it != prev->logic.end() && it->second) {
                for (size_t j = 0; j < prev->logic_ids.size(); ++j) {
                    if (prev->logic_ids[j] == id && j < prev->logic_addrs.size() &&
                        prev->logic_addrs[j] == logic_addrs[i]) {
                        built[id] = it->second;
                        break;
                    }
                }
                if (built.count(id))
                    continue;
            }
        }
        auto ch = std::make_shared<brpc::Channel>();
        brpc::ChannelOptions opt;
        opt.timeout_ms = timeout_ms_;
        opt.max_retry = 0;
        if (ch->Init(logic_addrs[i].c_str(), &opt) != 0) {
            LOG_ERROR << "GatewayAuthClients logic Init failed " << logic_addrs[i]
                      << " (abort publish, keep last snapshot)";
            return false;
        }
        built[id] = std::move(ch);
    }
    if (built.empty()) {
        LOG_ERROR << "GatewayAuthClients logic built empty (keep last snapshot)";
        return false;
    }

    auto next = std::make_shared<RpcChannelSnapshot>();
    if (prev) {
        next->session = prev->session;
        next->session_peer_count = prev->session_peer_count;
    }
    next->logic = std::move(built);
    next->logic_addrs = logic_addrs;
    next->logic_ids = std::move(ids);
    Publish(std::move(next));
    LOG_INFO << "GatewayAuthClients logic channels=" << CurrentSnapshot()->logic.size();
    return true;
}

std::shared_ptr<brpc::Channel> GatewayAuthClients::SharedLogicChannel(const std::string &id) {
    // fail-closed：未知 logic_server_id 绝不回退到首节点
    if (id.empty())
        return nullptr;
    auto s = CurrentSnapshot();
    if (!s)
        return nullptr;
    auto it = s->logic.find(id);
    if (it != s->logic.end())
        return it->second;
    LOG_ERROR << "GatewayAuthClients: unknown logic_server_id=" << id;
    return nullptr;
}

bool GatewayAuthClients::AuthLogin(const auth::LoginRequest &req, auth::LoginResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    const size_t tries = std::max<size_t>(1, s->session_peer_count);
    for (size_t i = 0; i < tries; ++i) {
        auth::AuthService_Stub stub(s->session.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms_);
        stub.Login(&cntl, &req, rsp, nullptr);
        if (!cntl.Failed())
            return true;
        if (i + 1 < tries)
            LOG_WARN << "GatewayAuthClients AuthLogin retry peer_fail=" << cntl.ErrorText();
    }
    return false;
}

bool GatewayAuthClients::AuthRegister(const auth::RegisterRequest &req, auth::RegisterResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    const size_t tries = std::max<size_t>(1, s->session_peer_count);
    for (size_t i = 0; i < tries; ++i) {
        auth::AuthService_Stub stub(s->session.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms_);
        stub.Register(&cntl, &req, rsp, nullptr);
        if (!cntl.Failed())
            return true;
        if (i + 1 < tries)
            LOG_WARN << "GatewayAuthClients AuthRegister retry peer_fail=" << cntl.ErrorText();
    }
    return false;
}

bool GatewayAuthClients::AcquireSession(const sess::AcquireSessionRequest &req,
                                        sess::AcquireSessionResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    // 变更类：框架 max_retry=0；多 peer 时应用层按 peer 数重试（依赖 operation_id 幂等）
    const size_t tries = std::max<size_t>(1, s->session_peer_count);
    for (size_t i = 0; i < tries; ++i) {
        sess::SessionService_Stub stub(s->session.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms_);
        stub.AcquireSession(&cntl, &req, rsp, nullptr);
        if (!cntl.Failed())
            return true;
        if (i + 1 < tries)
            LOG_WARN << "GatewayAuthClients AcquireSession retry peer_fail=" << cntl.ErrorText();
    }
    return false;
}

bool GatewayAuthClients::ReconnectV2(const sess::ReconnectRequest &req,
                                     sess::ReconnectResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    const size_t tries = std::max<size_t>(1, s->session_peer_count);
    for (size_t i = 0; i < tries; ++i) {
        sess::SessionService_Stub stub(s->session.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms_);
        stub.ReconnectV2(&cntl, &req, rsp, nullptr);
        if (!cntl.Failed())
            return true;
        if (i + 1 < tries)
            LOG_WARN << "GatewayAuthClients ReconnectV2 retry peer_fail=" << cntl.ErrorText();
    }
    return false;
}

bool GatewayAuthClients::PrepareReconnect(const sess::PrepareReconnectRequest &req,
                                          sess::PrepareReconnectResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    const size_t tries = std::max<size_t>(1, s->session_peer_count);
    for (size_t i = 0; i < tries; ++i) {
        sess::SessionService_Stub stub(s->session.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms_);
        stub.PrepareReconnect(&cntl, &req, rsp, nullptr);
        if (!cntl.Failed())
            return true;
        if (i + 1 < tries)
            LOG_WARN << "GatewayAuthClients PrepareReconnect retry peer_fail=" << cntl.ErrorText();
    }
    return false;
}

bool GatewayAuthClients::CommitReconnect(const sess::CommitReconnectRequest &req,
                                         sess::CommitReconnectResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    const size_t tries = std::max<size_t>(1, s->session_peer_count);
    for (size_t i = 0; i < tries; ++i) {
        sess::SessionService_Stub stub(s->session.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms_);
        stub.CommitReconnect(&cntl, &req, rsp, nullptr);
        if (!cntl.Failed())
            return true;
        if (i + 1 < tries)
            LOG_WARN << "GatewayAuthClients CommitReconnect retry peer_fail=" << cntl.ErrorText();
    }
    return false;
}

bool GatewayAuthClients::AbortReconnect(const sess::AbortReconnectRequest &req,
                                        sess::AbortReconnectResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.AbortReconnect(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::MarkDisconnectedV2(const sess::MarkDisconnectedRequest &req,
                                            sess::MarkDisconnectedResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.MarkDisconnectedV2(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::GetSessionOperation(const sess::GetSessionOperationRequest &req,
                                             sess::GetSessionOperationResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.GetSessionOperation(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::LogoutV2(const sess::LogoutRequest &req, sess::LogoutResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.LogoutV2(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::BeginPlayerTransfer(const sess::BeginPlayerTransferRequest &req,
                                             sess::BeginPlayerTransferResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.BeginPlayerTransfer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::CommitPlayerTransfer(const sess::CommitPlayerTransferRequest &req,
                                              sess::CommitPlayerTransferResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.CommitPlayerTransfer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::AbortPlayerTransfer(const sess::AbortPlayerTransferRequest &req,
                                             sess::AbortPlayerTransferResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.AbortPlayerTransfer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::GetPlayerRoute(const sess::GetPlayerRouteRequest &req,
                                        sess::GetPlayerRouteResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.GetPlayerRoute(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::UpdatePlayerRoute(const sess::UpdatePlayerRouteRequest &req,
                                           sess::UpdatePlayerRouteResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.UpdatePlayerRoute(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::ResolveOrCreateMap(const sess::ResolveOrCreateMapRequest &req,
                                            sess::ResolveOrCreateMapResponse *rsp) {
    auto s = CurrentSnapshot();
    if (!s || !s->session || !rsp)
        return false;
    sess::SessionService_Stub stub(s->session.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.ResolveOrCreateMap(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::BindPlayer(const std::string &logic_instance_id,
                                    const glrpc::BindPlayerRequest &req,
                                    glrpc::BindPlayerResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        glrpc::GameLogicService_Stub stub(ch.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms_);
        rsp->Clear();
        stub.BindPlayer(&cntl, &req, rsp, nullptr);
        if (!cntl.Failed() && rsp->ok())
            return true;
        if (!cntl.Failed())
            return false;
        LOG_WARN << "BindPlayer rpc failed logic=" << logic_instance_id << " attempt=" << attempt
                 << " err=" << cntl.ErrorText();
        if (attempt == 3)
            break;
        ::usleep(250000);
    }
    return false;
}

bool GatewayAuthClients::UnbindPlayer(const std::string &logic_instance_id,
                                      const glrpc::UnbindPlayerRequest &req,
                                      glrpc::UnbindPlayerResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.UnbindPlayer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

void GatewayAuthClients::UnbindPlayerAsync(const std::string &logic_instance_id,
                                           const glrpc::UnbindPlayerRequest &req) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch)
        return;
    struct UnbindCtx {
        brpc::Controller cntl;
        glrpc::UnbindPlayerRequest req;
        glrpc::UnbindPlayerResponse rsp;
        std::shared_ptr<brpc::Channel> channel_keep;
        std::string logic_id;
        uint64_t player_id = 0;
    };
    auto *ctx = new UnbindCtx();
    ctx->channel_keep = ch;
    ctx->logic_id = logic_instance_id;
    ctx->player_id = req.player_id();
    ctx->req = req;
    ctx->cntl.set_timeout_ms(timeout_ms_);
    glrpc::GameLogicService_Stub stub(ch.get());
    stub.UnbindPlayer(&ctx->cntl, &ctx->req, &ctx->rsp,
                      brpc::NewCallback(
                          +[](UnbindCtx *c) {
                              std::unique_ptr<UnbindCtx> guard(c);
                              if (c->cntl.Failed() || !c->rsp.ok()) {
                                  LOG_WARN << "UnbindPlayerAsync failed player=" << c->player_id
                                           << " logic=" << c->logic_id
                                           << " err="
                                           << (c->cntl.Failed() ? c->cntl.ErrorText()
                                                                : c->rsp.message());
                              }
                          },
                          ctx));
}

bool GatewayAuthClients::FreezePlayer(const std::string &logic_instance_id,
                                      const glrpc::FreezePlayerRequest &req,
                                      glrpc::FreezePlayerResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.FreezePlayer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::ExportPlayerSnapshot(const std::string &logic_instance_id,
                                              const glrpc::ExportPlayerSnapshotRequest &req,
                                              glrpc::ExportPlayerSnapshotResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.ExportPlayerSnapshot(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::ImportPlayerSnapshot(const std::string &logic_instance_id,
                                              const glrpc::ImportPlayerSnapshotRequest &req,
                                              glrpc::ImportPlayerSnapshotResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.ImportPlayerSnapshot(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::Dispatch(const std::string &logic_instance_id,
                                  const glrpc::ClientCommand &req, glrpc::CommandResult *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms_);
    stub.Dispatch(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}
