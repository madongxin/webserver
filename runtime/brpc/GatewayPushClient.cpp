#include "GatewayPushClient.h"

#include "Logging.h"

#include <brpc/channel.h>

GatewayPushClient &GatewayPushClient::Instance() {
    static GatewayPushClient g;
    return g;
}

void GatewayPushClient::SetGatewayPushAddr(const std::string &gateway_instance_id,
                                           const std::string &addr) {
    auto ch = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.timeout_ms = 2000;
    opt.max_retry = 0;
    if (ch->Init(addr.c_str(), &opt) != 0) {
        LOG_ERROR << "GatewayPushClient Init failed " << addr;
        return;
    }
    channels_[gateway_instance_id] = std::move(ch);
    LOG_INFO << "GatewayPushClient mapped " << gateway_instance_id << " -> " << addr;
}

bool GatewayPushClient::PushBatch(const std::string &gateway_instance_id,
                                  const gwpush::PushBatchRequest &req,
                                  gwpush::PushBatchResponse *rsp) {
    auto it = channels_.find(gateway_instance_id);
    if (it == channels_.end() || !rsp) {
        if (rsp) {
            rsp->set_ok(false);
            rsp->set_message("gateway push addr not configured");
        }
        return false;
    }
    gwpush::GatewayPushService_Stub stub(it->second.get());
    brpc::Controller cntl;
    stub.PushBatch(&cntl, &req, rsp, nullptr);
    if (cntl.Failed()) {
        rsp->set_ok(false);
        rsp->set_message(cntl.ErrorText());
        return false;
    }
    return rsp->ok();
}
