#pragma once

#include "ITransport.h"

#include <memory>
#include <string>
#include <vector>

namespace brpc {
class Channel;
}

/** Gateway → GameLogic / World 的 brpc 异步转发 */
class BrpcTransport : public ITransport {
public:
    static BrpcTransport &Instance();

    bool EnsureStarted(const std::vector<std::string> &logic_addrs, int timeout_ms = 3000);
    bool EnsureStarted(const std::vector<std::string> &logic_addrs,
                       const std::vector<std::string> &logic_instance_ids, int timeout_ms = 3000);
    bool EnsureStarted(const std::vector<std::string> &logic_addrs,
                       const std::vector<std::string> &logic_instance_ids,
                       const std::vector<std::string> &world_addrs, int timeout_ms = 3000);

    void PostPlayerRequest(const SessionHandle &handle,
                           std::string request_payload,
                           std::shared_ptr<ReplySink> sink) override;

    bool world_ready() const { return world_channel_ != nullptr; }

private:
    bool started_ = false;
    std::unique_ptr<brpc::Channel> world_channel_;
};
