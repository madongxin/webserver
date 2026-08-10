#pragma once

/**
 * @file MailService.h
 * @brief 邮件业务：投递、列表、已读、领取、容量、过期
 *
 * 玩家侧由 GameLogic 分发；投递可供本进程玩法直接调用。
 * 领取经 GameDB（AsyncMysqlGameDbRepository）异步事务 + 幂等 + outbox；
 * 提交成功后再改内存背包。
 */

#include "MailTypes.h"
#include "game.pb.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct GameDbMailClaimResult;

class MailService {
public:
    static MailService &Instance();

    bool Init();

    /** 系统投递（进程内 API）；幂等：同 source+business_key+receiver 只一封 */
    bool Deliver(const mail::DeliverRequest &req, uint64_t *mail_id, std::string *error_code,
                 std::string *message);

    bool HandleMailboxSummary(const game::MailboxSummaryReq &req, game::GameResponse *rsp);
    bool HandleMailList(const game::MailListReq &req, game::GameResponse *rsp);
    bool HandleMailGet(const game::MailGetReq &req, game::GameResponse *rsp);
    bool HandleMailRead(const game::MailReadReq &req, game::GameResponse *rsp);
    bool HandleMailClaim(const game::MailClaimReq &req, game::GameResponse *rsp);
    /**
     * 真异步领取：启动 GameDB RPC 后立即返回；完成后调用 done（已在 PlayerSerialQueue 上）。
     * @return true 已启动异步（调用方勿同步写 rsp）；false 同步失败已写入 *rsp
     */
    bool BeginHandleMailClaimAsync(const game::MailClaimReq &req, game::GameResponse *rsp,
                                   std::function<void(game::GameResponse)> done);
    /**
     * 真异步批量领取：逐封异步 GameDB，completion 回投 PlayerSerialQueue。
     * @return true 已启动；false 同步失败已写入 *rsp
     */
    bool BeginHandleMailBatchClaimAsync(const game::MailBatchClaimReq &req, game::GameResponse *rsp,
                                        std::function<void(game::GameResponse)> done);
    bool HandleMailBatchClaim(const game::MailBatchClaimReq &req, game::GameResponse *rsp);
    bool HandleMailFavorite(const game::MailFavoriteReq &req, game::GameResponse *rsp);
    bool HandleMailBatchRead(const game::MailBatchReadReq &req, game::GameResponse *rsp);
    bool HandleMailBatchDelete(const game::MailBatchDeleteReq &req, game::GameResponse *rsp);
    bool HandleMailDeliver(const game::MailDeliverReq &req, game::GameResponse *rsp);

    /** 定时/访问时：标记过期邮件 */
    int ScanExpire(int limit = 200);

    int64_t MailboxVersion(uint64_t player_id) const;

    /** 供异步 callback 在 PlayerSerialQueue 上应用内存奖励 */
    void ApplyClaimMemory(uint64_t player_id, const GameDbMailClaimResult &db_rsp);

private:
    MailService() = default;

    int64_t NowUtc() const;
    void BumpVersion(uint64_t player_id);
    bool EnsureCapacityForDeliver(uint64_t receiver_id, std::string *error_code,
                                  std::string *message);
    bool SoftDeleteMail(uint64_t mail_id, uint64_t actor_id, int64_t now);
    bool TouchExpireIfNeeded(mail::MailInstanceRow *row, int64_t now);

    bool ClaimOne(uint64_t player_id, uint64_t mail_id, const std::string &idempotency_key,
                  const std::string &trace_id, game::MailClaimResult *result);

    void FillMailBrief(const mail::MailInstanceRow &row, game::MailBrief *out);
    void FillMailDetail(const mail::MailInstanceRow &row,
                        const std::vector<mail::MailAttachmentRow> &atts, game::MailDetail *out);

    mutable std::mutex ver_mu_;
    std::map<uint64_t, int64_t> mailbox_version_;
    bool ready_ = false;
};
