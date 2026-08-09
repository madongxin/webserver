#pragma once

#include "IGameDbRepository.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace brpc {
class Channel;
}

/** World/Logic 侧：经 brpc 调用独立 GameDB；支持多地址 + ApplySnapshot */
class BrpcGameDbRepository : public IGameDbRepository {
public:
    static BrpcGameDbRepository &Instance();

    bool Init(const std::string &addr, int timeout_ms = 3000);
    bool Init(const std::vector<std::string> &addrs, int timeout_ms = 3000);
    /** 热更新 Channel 快照；空列表保留现有（不 fail-open 清空） */
    bool ApplySnapshot(const std::vector<std::string> &addrs);
    void Start(int worker_count = 0) override;
    void Stop() override;
    bool started() const override { return started_; }
    size_t channel_count() const;

    void ClaimMailAttachmentsAsync(GameDbMailClaimRequest req, MailClaimDone done) override;
    GameDbMailClaimResult ClaimMailAttachments(GameDbMailClaimRequest req);

    struct AssetMutationResult {
        bool ok = false;
        bool idempotent_hit = false;
        std::string error_code;
        std::string message;
        uint64_t asset_version = 0;
        uint32_t remain_count = 0;
    };
    bool LoadPlayer(uint64_t player_id, uint64_t *asset_version, bool *exists, std::string *err);
    bool LoadInventory(uint64_t player_id, std::map<uint32_t, uint32_t> *bag, uint64_t *version,
                       std::string *err);
    bool ApplyAssetMutation(uint64_t player_id, const std::string &idempotency_key,
                            uint64_t expected_version, const std::string &mutation_type,
                            uint32_t item_id, uint32_t count, const std::string &trace_id,
                            AssetMutationResult *out);
    bool SavePlayerSnapshot(uint64_t player_id, uint64_t expected_version,
                            const std::map<uint32_t, uint32_t> &bag,
                            const std::string &idempotency_key, uint64_t *new_version,
                            std::string *err);
    bool FlushPlayer(uint64_t player_id, const std::string &reason, uint64_t *version,
                     std::string *err);

private:
    BrpcGameDbRepository() = default;
    std::shared_ptr<brpc::Channel> ChannelForPlayer(uint64_t player_id);
    /** 幂等读：首通道失败可试下一通道 */
    std::shared_ptr<brpc::Channel> ChannelAt(size_t idx);

    mutable std::mutex mu_;
    std::vector<std::shared_ptr<brpc::Channel>> channels_;
    bool started_ = false;
    int timeout_ms_ = 3000;
};
