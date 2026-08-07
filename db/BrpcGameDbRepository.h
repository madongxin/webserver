#pragma once

#include "IGameDbRepository.h"

#include <memory>
#include <string>
#include <vector>

namespace brpc {
class Channel;
}

/** World/Logic 侧：经 brpc 调用独立 GameDB；支持多地址 player_id % N */
class BrpcGameDbRepository : public IGameDbRepository {
public:
    static BrpcGameDbRepository &Instance();

    bool Init(const std::string &addr, int timeout_ms = 3000);
    bool Init(const std::vector<std::string> &addrs, int timeout_ms = 3000);
    void Start(int worker_count = 0) override;
    void Stop() override;
    bool started() const override { return started_; }
    size_t channel_count() const { return channels_.size(); }

    void ClaimMailAttachmentsAsync(GameDbMailClaimRequest req, MailClaimDone done) override;
    GameDbMailClaimResult ClaimMailAttachments(GameDbMailClaimRequest req);

private:
    BrpcGameDbRepository() = default;
    brpc::Channel *ChannelForPlayer(uint64_t player_id);

    std::vector<std::unique_ptr<brpc::Channel>> channels_;
    bool started_ = false;
    int timeout_ms_ = 3000;
};
