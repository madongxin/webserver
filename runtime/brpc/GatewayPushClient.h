#pragma once

#include "RpcChannelSnapshot.h"
#include "gateway_push.pb.h"

#include <atomic>
#include <memory>
#include <string>

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
    uint64_t snapshot_version() const;

private:
    GatewayPushClient() = default;
    std::shared_ptr<const GatewayPushSnapshot> Current() const;
    void Publish(std::shared_ptr<GatewayPushSnapshot> next);

    std::shared_ptr<const GatewayPushSnapshot> snap_{std::make_shared<GatewayPushSnapshot>()};
    std::atomic<uint64_t> version_{0};
};
