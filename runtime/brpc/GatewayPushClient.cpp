#include "GatewayPushClient.h"

#include "IServiceRegistry.h"
#include "Logging.h"

#include <brpc/callback.h>
#include <brpc/channel.h>

#include <atomic>
#include <memory>
#include <vector>

GatewayPushClient &GatewayPushClient::Instance() {
    static GatewayPushClient g;
    return g;
}

std::shared_ptr<const GatewayPushSnapshot> GatewayPushClient::Current() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
}

void GatewayPushClient::Publish(std::shared_ptr<GatewayPushSnapshot> next) {
    if (!next)
        return;
    next->version = version_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::atomic_store_explicit(&snap_, std::shared_ptr<const GatewayPushSnapshot>(std::move(next)),
                               std::memory_order_release);
}

uint64_t GatewayPushClient::snapshot_version() const {
    auto s = Current();
    return s ? s->version : 0;
}

void GatewayPushClient::SetGatewayPushAddr(const std::string &gateway_instance_id,
                                           const std::string &addr) {
    if (gateway_instance_id.empty() || addr.empty())
        return;
    // 拒绝把 listen 通配地址当 push 端点
    if (addr.rfind("0.0.0.0:", 0) == 0) {
        LOG_WARN << "GatewayPushClient refuse 0.0.0.0 advertise for " << gateway_instance_id;
        return;
    }

    auto prev = Current();
    if (prev) {
        auto ait = prev->addrs.find(gateway_instance_id);
        auto cit = prev->by_gateway_id.find(gateway_instance_id);
        if (ait != prev->addrs.end() && ait->second == addr && cit != prev->by_gateway_id.end() &&
            cit->second) {
            return;  // 无变化
        }
    }

    auto ch = std::make_shared<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.timeout_ms = 2000;
    opt.max_retry = 0;
    if (ch->Init(addr.c_str(), &opt) != 0) {
        LOG_ERROR << "GatewayPushClient Init failed " << addr << " (keep last snapshot)";
        return;
    }

    auto next = std::make_shared<GatewayPushSnapshot>();
    if (prev) {
        next->by_gateway_id = prev->by_gateway_id;
        next->addrs = prev->addrs;
    }
    next->by_gateway_id[gateway_instance_id] = std::move(ch);
    next->addrs[gateway_instance_id] = addr;
    Publish(std::move(next));
    LOG_INFO << "GatewayPushClient mapped " << gateway_instance_id << " -> " << addr;
}

bool GatewayPushClient::EnsureChannel(const std::string &gateway_instance_id) {
    {
        auto s = Current();
        if (s && s->by_gateway_id.count(gateway_instance_id))
            return true;
    }
    std::vector<IServiceRegistry::ServiceInstance> insts;
    if (!ServiceRegistryFacade::Get().Active().Discover("gateway_push", &insts))
        return false;
    for (const auto &inst : insts) {
        if (inst.instance_id == gateway_instance_id && !inst.address.empty()) {
            SetGatewayPushAddr(gateway_instance_id, inst.address);
            break;
        }
    }
    auto s = Current();
    return s && s->by_gateway_id.count(gateway_instance_id) > 0;
}

bool GatewayPushClient::PushBatch(const std::string &gateway_instance_id,
                                  const gwpush::PushBatchRequest &req,
                                  gwpush::PushBatchResponse *rsp) {
    if (!rsp)
        return false;
    if (!EnsureChannel(gateway_instance_id)) {
        rsp->set_ok(false);
        rsp->set_message("gateway push addr unknown (no discovery/static mapping)");
        return false;
    }
    auto s = Current();
    if (!s) {
        rsp->set_ok(false);
        rsp->set_message("gateway push snapshot missing");
        return false;
    }
    auto it = s->by_gateway_id.find(gateway_instance_id);
    if (it == s->by_gateway_id.end() || !it->second) {
        rsp->set_ok(false);
        rsp->set_message("gateway push channel missing");
        return false;
    }
    auto ch = it->second;  // 持有在途
    gwpush::GatewayPushService_Stub stub(ch.get());
    brpc::Controller cntl;
    stub.PushBatch(&cntl, &req, rsp, nullptr);
    if (cntl.Failed()) {
        rsp->set_ok(false);
        rsp->set_message(cntl.ErrorText());
        return false;
    }
    return rsp->ok();
}

void GatewayPushClient::KickConnectionAsync(const std::string &gateway_instance_id,
                                            uint64_t player_id, const std::string &session_id,
                                            uint64_t generation, const std::string &reason) {
    if (gateway_instance_id.empty() || player_id == 0)
        return;
    if (!EnsureChannel(gateway_instance_id)) {
        LOG_WARN << "KickConnectionAsync no channel gw=" << gateway_instance_id
                 << " player=" << player_id;
        return;
    }
    auto s = Current();
    if (!s)
        return;
    auto it = s->by_gateway_id.find(gateway_instance_id);
    if (it == s->by_gateway_id.end() || !it->second)
        return;
    struct KickCtx {
        brpc::Controller cntl;
        gwpush::KickConnectionRequest req;
        gwpush::KickConnectionResponse rsp;
        std::shared_ptr<brpc::Channel> ch;
        std::string gw;
        uint64_t player_id = 0;
    };
    auto *ctx = new KickCtx();
    ctx->ch = it->second;
    ctx->gw = gateway_instance_id;
    ctx->player_id = player_id;
    ctx->req.set_player_id(player_id);
    ctx->req.set_session_id(session_id);
    ctx->req.set_generation(generation);
    ctx->req.set_reason(reason);
    gwpush::GatewayPushService_Stub stub(ctx->ch.get());
    stub.KickConnection(&ctx->cntl, &ctx->req, &ctx->rsp,
                        brpc::NewCallback(
                            +[](KickCtx *c) {
                                std::unique_ptr<KickCtx> guard(c);
                                if (c->cntl.Failed() || !c->rsp.ok()) {
                                    LOG_WARN << "KickConnectionAsync failed gw=" << c->gw
                                             << " player=" << c->player_id << " err="
                                             << (c->cntl.Failed() ? c->cntl.ErrorText()
                                                                  : c->rsp.message());
                                }
                            },
                            ctx));
}
