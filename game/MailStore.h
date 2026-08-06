#pragma once

/**
 * @file MailStore.h
 * @brief 邮件表 MySQL 访问（raw SQL，与 PlayerItemStore 风格一致）
 */

#include "MailTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Connection;

struct MailListFilter {
    std::string category;  // 空=全部
    bool only_unread = false;
    bool only_has_attachment = false;
    bool only_unclaimed = false;
    bool only_expiring_soon = false;  // 72h 内
    bool only_favorite = false;
    std::string keyword;
    std::string cursor;
    int limit = 20;
};

struct MailListItem {
    mail::MailInstanceRow row;
};

struct MailOpLogRow {
    uint64_t operation_id = 0;
    uint64_t mail_id = 0;
    uint64_t actor_id = 0;
    std::string operation_type;
    std::string idempotency_key;
    std::string before_state;
    std::string after_state;
    std::string result_code;
    std::string trace_id;
    int64_t created_at = 0;
};

class MailStore {
public:
    static MailStore &Instance();

    bool EnsureTables();
    bool Available() const { return available_; }

    bool Begin(Connection *conn);
    bool Commit(Connection *conn);
    bool Rollback(Connection *conn);

    /** 投递插入；冲突时返回 false 且 *duplicate=true */
    bool InsertMail(Connection *conn, const mail::MailInstanceRow &row, uint64_t *mail_id,
                    bool *duplicate);
    bool InsertAttachment(Connection *conn, const mail::MailAttachmentRow &row);

    bool CountActiveMails(uint64_t receiver_id, int *count);
    bool CountFavorites(uint64_t receiver_id, int *count);

    bool LoadMail(uint64_t mail_id, mail::MailInstanceRow *out);
    bool LoadMailForUpdate(Connection *conn, uint64_t mail_id, mail::MailInstanceRow *out);
    bool LoadAttachments(uint64_t mail_id, std::vector<mail::MailAttachmentRow> *out);
    bool LoadAttachmentsForUpdate(Connection *conn, uint64_t mail_id,
                                  std::vector<mail::MailAttachmentRow> *out);

    bool UpdateMailRow(Connection *conn, const mail::MailInstanceRow &row);
    bool UpdateAttachmentsClaimed(Connection *conn, uint64_t mail_id,
                                  const std::string &asset_tx_id, int64_t claimed_at);
    bool UpdateAttachmentsClaimState(Connection *conn, uint64_t mail_id,
                                     const std::string &claim_state);

    bool ListMails(uint64_t receiver_id, const MailListFilter &filter,
                   std::vector<MailListItem> *out, std::string *next_cursor);
    bool Summarize(uint64_t receiver_id, int64_t now_utc, int64_t mailbox_version,
                   int *unread_system, int *unread_activity, int *unread_social,
                   int *unread_trade, int *unclaimed_attach, int *expiring_soon,
                   int *current_count);

    bool FindOpByIdempotency(const std::string &key, MailOpLogRow *out);
    bool InsertOpLog(Connection *conn, const MailOpLogRow &row);

    /** 自动清理候选：按规则排序的 mail_id 列表 */
    bool SelectAutoCleanupCandidates(uint64_t receiver_id, int64_t now_utc, int protect_hours,
                                     int limit, std::vector<uint64_t> *mail_ids);

    bool MarkExpiredBatch(int64_t now_utc, int limit, int *affected);

    /** 按幂等键查已存在投递 */
    bool FindByBusinessKey(const std::string &source_system, const std::string &business_key,
                           uint64_t receiver_id, mail::MailInstanceRow *out);

    std::shared_ptr<Connection> GetConnection();

private:
    MailStore() = default;
    bool available_ = false;
    bool tables_ready_ = false;
};
