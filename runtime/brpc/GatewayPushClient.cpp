#include "GatewayPushClient.h"

#include "IServiceRegistry.h"
#include "Logging.h"

#include <brpc/channel.h>

#include <mutex>

namespace {
std::mutex g_push_mu;
}

GatewayPushClient &GatewayPushClient::Instance() {
    static GatewayPushClient g;
    return g;
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
    auto ch = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.timeout_ms = 2000;
    opt.max_retry = 0;
    if (ch->Init(addr.c_str(), &opt) != 0) {
        LOG_ERROR << "GatewayPushClient Init failed " << addr;
        return;
    }
    std::lock_guard<std::mutex> lk(g_push_mu);
    channels_[gateway_instance_id] = std::move(ch);
    LOG_INFO << "GatewayPushClient mapped " << gateway_instance_id << " -> " << addr;
}

bool GatewayPushClient::EnsureChannel(const std::string &gateway_instance_id) {
    {
        std::lock_guard<std::mutex> lk(g_push_mu);
        if (channels_.count(gateway_instance_id))
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
    std::lock_guard<std::mutex> lk(g_push_mu);
    return channels_.count(gateway_instance_id) > 0;
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
    brpc::Channel *ch = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_push_mu);
        auto it = channels_.find(gateway_instance_id);
        if (it == channels_.end()) {
            rsp->set_ok(false);
            rsp->set_message("gateway push channel missing");
            return false;
        }
        ch = it->second.get();
    }
    gwpush::GatewayPushService_Stub stub(ch);
    brpc::Controller cntl;
    stub.PushBatch(&cntl, &req, rsp, nullptr);
    if (cntl.Failed()) {
        rsp->set_ok(false);
        rsp->set_message(cntl.ErrorText());
        return false;
    }
    return rsp->ok();
}
