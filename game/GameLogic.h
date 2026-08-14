#pragma once

#include "game.pb.h"
#include "IGameDbRepository.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/** 游戏业务单例：由 GameService::HandleFrame 在反序列化 GameRequest 后调用 */
class GameLogic {
public:
    static GameLogic &Instance();
    /** 按 oneof body 分发到各 HandleXxx；填充 GameResponse（含 seq） */
    bool Handle(const game::GameRequest &req, game::GameResponse *rsp);

    /** 邮件领取等：查询内存背包聚合数量 */
    uint32_t GetItemCount(uint64_t player_id, uint32_t item_id);
    /** 提交 GameDB 前拷贝背包快照（供 worker 软上限校验） */
    void CopyInventory(uint64_t player_id, std::unordered_map<uint32_t, uint32_t> *out);
    /** 邮件领取事务提交后同步增加内存背包 */
    bool ApplyItemReward(uint64_t player_id, uint32_t item_id, uint32_t count);
    /**
     * 邮件领取成功：同锁内应用 grants 并设为 DB 提交后的 asset_version（禁止回退）。
     * 版本无法衔接时在锁外从 GameDB 重载完整背包。
     */
    bool ApplyItemRewardsWithVersion(uint64_t player_id, const std::vector<GameDbGrantedItem> &grants,
                                     uint64_t committed_asset_version);
    /** 当前内存资产版本（测试/调试） */
    uint64_t GetAssetVersion(uint64_t player_id);
    bool IsAssetDirty(uint64_t player_id);
    /** dirty 时尝试权威重载；成功后允许资产写 */
    bool EnsureAssetsSynced(uint64_t player_id);
    void SetAssetReloadBlockedForTest(bool v);
    void SetAssetApplyBlockedForTest(bool v);
    void SetAssetReloadOverrideForTest(const std::map<uint32_t, uint32_t> &bag, uint64_t ver);
    void ClearAssetReloadOverrideForTest();
    bool HandleConsumeItemForTest(const game::ConsumeItemReq &req, game::GameResponse *rsp) {
        return HandleConsumeItem(req, rsp);
    }
    bool HandleGrantItemForTest(const game::GrantItemReq &req, game::GameResponse *rsp) {
        return HandleGrantItem(req, rsp);
    }
    /** 事务失败时回退内存（尽力而为） */
    bool RollbackItemReward(uint64_t player_id, uint32_t item_id, uint32_t count);

    /** Gateway BindPlayer：已认证玩家加载内存态（不接收凭证）；失败 fail-closed */
    bool BindAuthenticatedPlayer(uint64_t player_id, std::string *err = nullptr);
    bool FlushBag(uint64_t player_id, const std::string &reason);

    /** 跨 Logic 迁移：导出/导入运行时背包与技能 CD（不含凭证） */
    bool ExportRuntimeState(uint64_t player_id, std::map<uint32_t, uint32_t> *bag,
                            std::map<uint32_t, int64_t> *skill_cds, uint64_t *asset_version);
    bool ImportRuntimeState(uint64_t player_id, const std::map<uint32_t, uint32_t> &bag,
                            const std::map<uint32_t, int64_t> &skill_cds, uint64_t asset_version);
    /** 全量快照（Push 缺口）：填充 FullStateSnapshotRsp */
    bool BuildFullStateSnapshot(uint64_t player_id, game::FullStateSnapshotRsp *out);

private:
    GameLogic() = default;
    bool HandleConsumeItem(const game::ConsumeItemReq &req, game::GameResponse *rsp);
    bool HandleReleaseSkill(const game::ReleaseSkillReq &req, game::GameResponse *rsp);
    /** 发放道具：内存即时生效，MySQL 经 PlayerItemPersistQueue 异步落库 */
    bool HandleGrantItem(const game::GrantItemReq &req, game::GameResponse *rsp);
    bool HandleLogin(const game::LoginReq &req, game::GameResponse *rsp);
    bool HandleReconnect(const game::ReconnectReq &req, game::GameResponse *rsp);
    bool HandleValidateSession(const game::ValidateSessionReq &req, game::GameResponse *rsp);
    bool HandleCheckOnline(const game::CheckOnlineReq &req, game::GameResponse *rsp);
    bool HandleLogout(const game::LogoutReq &req, game::GameResponse *rsp);
    bool HandleRegister(const game::RegisterReq &req, game::GameResponse *rsp);
    bool HandleFlushBag(const game::FlushBagReq &req, game::GameResponse *rsp);
    bool HandleEnterMap(const game::EnterMapReq &req, game::GameResponse *rsp);
    bool HandleLeaveMap(const game::LeaveMapReq &req, game::GameResponse *rsp);
    bool HandleMapPing(const game::MapPingReq &req, game::GameResponse *rsp);
    bool HandleChatSend(const game::ChatSendReq &req, game::GameResponse *rsp);
    bool HandleFriendList(const game::FriendListReq &req, game::GameResponse *rsp);
    bool RequireSessionToken(const game::GameRequest &req, uint64_t player_id, game::GameResponse *rsp);
    /** @return false：Formal 下 GameDB 加载失败，不得当作空背包成功 */
    bool EnsurePlayerLoaded(uint64_t player_id, std::string *err);
    void EnsurePlayer(uint64_t player_id);
    /** 从正式资产重载背包+版本（不得在持 mu_ 时调用远程 IO） */
    bool LoadAssetsFromGameDbUnlocked(uint64_t player_id, std::map<uint32_t, uint32_t> *bag,
                                      uint64_t *ver);
    bool TryReloadAssets(uint64_t player_id);

    std::mutex mu_;
    /** 内存背包：player_id -> (item_config_id -> 聚合数量），与 consume_item 共用 */
    std::map<uint64_t, std::map<uint32_t, uint32_t>> inventory_;
    std::map<uint64_t, std::map<uint32_t, int64_t>> skill_cd_until_ms_;
    std::map<uint64_t, uint64_t> asset_version_;
    std::map<uint64_t, bool> player_load_ok_;
    std::map<uint64_t, bool> asset_dirty_;
    bool reload_blocked_for_test_ = false;
    bool apply_blocked_for_test_ = false;
    bool reload_override_for_test_ = false;
    std::map<uint32_t, uint32_t> reload_override_bag_;
    uint64_t reload_override_ver_ = 0;
};
