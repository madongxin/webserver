#pragma once

#include "gateway_push.pb.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace brpc {
class Channel;
}

class GatewayPushClient {
public:
    static GatewayPushClient &Instance();
    void SetGatewayPushAddr(const std::string &gateway_instance_id, const std::string &addr);
    /** 若未配置则从 IServiceRegistry(gateway_push) 解析 */
    bool EnsureChannel(const std::string &gateway_instance_id);
    bool PushBatch(const std::string &gateway_instance_id, const gwpush::PushBatchRequest &req,
                   gwpush::PushBatchResponse *rsp);

private:
    GatewayPushClient() = default;
    std::unordered_map<std::string, std::unique_ptr<brpc::Channel>> channels_;
};
