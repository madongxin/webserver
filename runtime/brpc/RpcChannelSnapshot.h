#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace brpc {
class Channel;
}

/** 不可变 RPC Channel 快照：构建完成后再原子发布；在途 RPC 持有 shared_ptr。 */
struct RpcChannelSnapshot {
    std::shared_ptr<brpc::Channel> session;
    size_t session_peer_count = 0;
    std::unordered_map<std::string, std::shared_ptr<brpc::Channel>> logic;
    std::vector<std::string> logic_addrs;
    std::vector<std::string> logic_ids;
    uint64_t version = 0;
};

struct LogicChannelPoolSnapshot {
    std::vector<std::shared_ptr<brpc::Channel>> channels;
    std::vector<std::string> instance_ids;
    std::vector<std::string> addrs;
    std::unordered_map<std::string, size_t> id_to_index;
    int timeout_ms = 3000;
    uint64_t version = 0;
};

struct GatewayPushSnapshot {
    std::unordered_map<std::string, std::shared_ptr<brpc::Channel>> by_gateway_id;
    std::unordered_map<std::string, std::string> addrs;
    uint64_t version = 0;
};
