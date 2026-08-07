#include "BrpcChannelManager.h"

#include "Logging.h"

#include <brpc/channel.h>

#include <sstream>

BrpcChannelManager &BrpcChannelManager::Instance() {
    static BrpcChannelManager g;
    return g;
}

BrpcChannelManager::~BrpcChannelManager() {
    Shutdown();
}

bool BrpcChannelManager::Init(const std::vector<std::string> &addrs,
                              const std::vector<std::string> &instance_ids, int timeout_ms) {
    Shutdown();
    if (addrs.empty()) {
        LOG_ERROR << "BrpcChannelManager: empty addrs";
        return false;
    }
    std::vector<std::string> ids = instance_ids;
    if (ids.empty()) {
        for (size_t i = 0; i < addrs.size(); ++i) {
            std::ostringstream os;
            os << "gl-" << i;
            ids.push_back(os.str());
        }
    } else if (ids.size() != addrs.size()) {
        LOG_ERROR << "BrpcChannelManager: instance_ids size mismatch addrs=" << addrs.size()
                  << " ids=" << ids.size();
        return false;
    }
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 3000;
    channels_.reserve(addrs.size());
    for (size_t i = 0; i < addrs.size(); ++i) {
        auto ch = std::make_unique<brpc::Channel>();
        brpc::ChannelOptions opt;
        opt.protocol = "baidu_std";
        opt.timeout_ms = timeout_ms_;
        opt.max_retry = 0;
        if (ch->Init(addrs[i].c_str(), &opt) != 0) {
            LOG_ERROR << "BrpcChannelManager: Channel::Init failed addr=" << addrs[i];
            channels_.clear();
            id_to_index_.clear();
            instance_ids_.clear();
            return false;
        }
        LOG_INFO << "BrpcChannelManager: channel ready id=" << ids[i] << " addr=" << addrs[i]
                 << " timeout_ms=" << timeout_ms_;
        id_to_index_[ids[i]] = i;
        channels_.push_back(std::move(ch));
    }
    instance_ids_ = std::move(ids);
    return true;
}

void BrpcChannelManager::Shutdown() {
    channels_.clear();
    instance_ids_.clear();
    id_to_index_.clear();
}

brpc::Channel *BrpcChannelManager::ChannelForPlayer(uint64_t player_id) {
    if (channels_.empty())
        return nullptr;
    const size_t idx = static_cast<size_t>(player_id % channels_.size());
    return channels_[idx].get();
}

brpc::Channel *BrpcChannelManager::ChannelForInstance(const std::string &gamelogic_instance_id) {
    if (channels_.empty() || gamelogic_instance_id.empty())
        return nullptr;
    auto it = id_to_index_.find(gamelogic_instance_id);
    if (it == id_to_index_.end())
        return nullptr;
    return channels_[it->second].get();
}
