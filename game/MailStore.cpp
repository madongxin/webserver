#include "MailStore.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"

#include <mysql/mysql.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace {

std::mutex g_mu;

std::string SqlEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '\'')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

int64_t CellI64(MYSQL_ROW row, unsigned idx) {
    if (!row || !row[idx])
        return 0;
    return static_cast<int64_t>(std::strtoll(row[idx], nullptr, 10));
}

uint64_t CellU64(MYSQL_ROW row, unsigned idx) {
    if (!row || !row[idx])
        return 0;
    return static_cast<uint64_t>(std::strtoull(row[idx], nullptr, 10));
}

std::string CellStr(MYSQL_ROW row, unsigned idx) {
    if (!row || !row[idx])
        return "";
    return row[idx];
}

bool FillMailRow(MYSQL_ROW row, mail::MailInstanceRow *out) {
    if (!row || !out)
        return false;
    out->mail_id = CellU64(row, 0);
    out->owner_scope = CellStr(row, 1);
    out->receiver_id = CellU64(row, 2);
    out->sender_type = CellStr(row, 3);
    out->sender_id = CellU64(row, 4);
    out->source_system = CellStr(row, 5);
    out->business_key = CellStr(row, 6);
    out->template_id = CellStr(row, 7);
    out->template_version = static_cast<int>(CellI64(row, 8));
    out->category = CellStr(row, 9);
    out->priority = static_cast<int>(CellI64(row, 10));
    out->sender_name_snapshot = CellStr(row, 11);
    out->title_snapshot = CellStr(row, 12);
    out->body_snapshot = CellStr(row, 13);
    out->read_state = CellStr(row, 14);
    out->read_at = CellI64(row, 15);
    out->visible_state = CellStr(row, 16);
    out->has_attachment = CellI64(row, 17) != 0;
    out->attachment_state = CellStr(row, 18);
    out->is_favorite = CellI64(row, 19) != 0;
    out->sent_at = CellI64(row, 20);
    out->expire_at = CellI64(row, 21);
    out->deleted_at = CellI64(row, 22);
    out->row_version = CellI64(row, 23);
    out->created_at = CellI64(row, 24);
    out->updated_at = CellI64(row, 25);
    return out->mail_id > 0;
}

const char *kMailSelectCols =
    "mail_id,owner_scope,receiver_id,sender_type,sender_id,source_system,business_key,"
    "template_id,template_version,category,priority,sender_name_snapshot,title_snapshot,"
    "body_snapshot,read_state,read_at,visible_state,has_attachment,attachment_state,"
    "is_favorite,sent_at,expire_at,deleted_at,row_version,created_at,updated_at";

}  // namespace

MailStore &MailStore::Instance() {
    static MailStore g;
    return g;
}

std::shared_ptr<Connection> MailStore::GetConnection() {
    if (!ConnectionPool::getconnectionPool()->isInitialized())
        return nullptr;
    return ConnectionPool::getconnectionPool()->getConnection();
}

bool MailStore::Begin(Connection *conn) {
    return conn && conn->update("START TRANSACTION");
}
bool MailStore::Commit(Connection *conn) {
    return conn && conn->update("COMMIT");
}
bool MailStore::Rollback(Connection *conn) {
    return conn && conn->update("ROLLBACK");
}

bool MailStore::EnsureTables() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (tables_ready_)
        return true;
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        available_ = false;
        return false;
    }
    auto conn = GetConnection();
    if (!conn)
        return false;

    const char *sql_mail =
        "CREATE TABLE IF NOT EXISTS mail_instance ("
        "mail_id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "owner_scope VARCHAR(16) NOT NULL DEFAULT 'ROLE',"
        "receiver_id BIGINT NOT NULL,"
        "sender_type VARCHAR(16) NOT NULL DEFAULT 'SYSTEM',"
        "sender_id BIGINT NOT NULL DEFAULT 0,"
        "source_system VARCHAR(64) NOT NULL,"
        "business_key VARCHAR(128) NOT NULL,"
        "template_id VARCHAR(64) NOT NULL DEFAULT '',"
        "template_version INT NOT NULL DEFAULT 1,"
        "category VARCHAR(16) NOT NULL DEFAULT 'SYSTEM',"
        "priority INT NOT NULL DEFAULT 0,"
        "sender_name_snapshot VARCHAR(64) NOT NULL DEFAULT '',"
        "title_snapshot VARCHAR(256) NOT NULL DEFAULT '',"
        "body_snapshot TEXT NOT NULL,"
        "read_state VARCHAR(16) NOT NULL DEFAULT 'UNREAD',"
        "read_at BIGINT NULL,"
        "visible_state VARCHAR(16) NOT NULL DEFAULT 'ACTIVE',"
        "has_attachment TINYINT NOT NULL DEFAULT 0,"
        "attachment_state VARCHAR(16) NOT NULL DEFAULT 'NONE',"
        "is_favorite TINYINT NOT NULL DEFAULT 0,"
        "sent_at BIGINT NOT NULL,"
        "expire_at BIGINT NOT NULL,"
        "deleted_at BIGINT NULL,"
        "row_version BIGINT NOT NULL DEFAULT 1,"
        "created_at BIGINT NOT NULL,"
        "updated_at BIGINT NOT NULL,"
        "UNIQUE KEY uk_mail_deliver (source_system, business_key, receiver_id),"
        "KEY idx_mail_receiver_vis_sent (receiver_id, visible_state, sent_at),"
        "KEY idx_mail_receiver_cat_read (receiver_id, category, read_state),"
        "KEY idx_mail_expire (expire_at),"
        "KEY idx_mail_attach_state (attachment_state),"
        "KEY idx_mail_favorite (receiver_id, is_favorite)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    const char *sql_attach =
        "CREATE TABLE IF NOT EXISTS mail_attachment ("
        "attachment_id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "mail_id BIGINT NOT NULL,"
        "slot_index INT NOT NULL,"
        "asset_type VARCHAR(16) NOT NULL DEFAULT 'ITEM',"
        "asset_id BIGINT NOT NULL,"
        "count INT NOT NULL DEFAULT 1,"
        "bind_type VARCHAR(16) NOT NULL DEFAULT 'NONE',"
        "payload TEXT NOT NULL,"
        "claim_state VARCHAR(16) NOT NULL DEFAULT 'UNCLAIMED',"
        "asset_transaction_id VARCHAR(64) NOT NULL DEFAULT '',"
        "claimed_at BIGINT NULL,"
        "created_at BIGINT NOT NULL,"
        "updated_at BIGINT NOT NULL,"
        "UNIQUE KEY uk_mail_slot (mail_id, slot_index),"
        "KEY idx_mail_attach_mail (mail_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    const char *sql_log =
        "CREATE TABLE IF NOT EXISTS mail_operation_log ("
        "operation_id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "mail_id BIGINT NOT NULL DEFAULT 0,"
        "actor_id BIGINT NOT NULL,"
        "operation_type VARCHAR(32) NOT NULL,"
        "idempotency_key VARCHAR(128) NOT NULL,"
        "before_state TEXT NOT NULL,"
        "after_state TEXT NOT NULL,"
        "result_code VARCHAR(64) NOT NULL,"
        "trace_id VARCHAR(64) NOT NULL DEFAULT '',"
        "created_at BIGINT NOT NULL,"
        "UNIQUE KEY uk_mail_op_idem (idempotency_key),"
        "KEY idx_mail_op_mail (mail_id),"
        "KEY idx_mail_op_actor (actor_id, created_at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (!conn->update(sql_mail) || !conn->update(sql_attach) || !conn->update(sql_log))
        return false;
    tables_ready_ = true;
    available_ = true;
    LOG_INFO << "MailStore: mail tables ready";
    return true;
}

bool MailStore::InsertMail(Connection *conn, const mail::MailInstanceRow &row, uint64_t *mail_id,
                           bool *duplicate) {
    if (!conn || !mail_id || !duplicate)
        return false;
    *duplicate = false;
    std::ostringstream sql;
    sql << "INSERT INTO mail_instance ("
        << "owner_scope,receiver_id,sender_type,sender_id,source_system,business_key,"
        << "template_id,template_version,category,priority,sender_name_snapshot,"
        << "title_snapshot,body_snapshot,read_state,read_at,visible_state,has_attachment,"
        << "attachment_state,is_favorite,sent_at,expire_at,deleted_at,row_version,"
        << "created_at,updated_at) VALUES ("
        << "'" << SqlEscape(row.owner_scope) << "'," << row.receiver_id << ","
        << "'" << SqlEscape(row.sender_type) << "'," << row.sender_id << ","
        << "'" << SqlEscape(row.source_system) << "','" << SqlEscape(row.business_key) << "',"
        << "'" << SqlEscape(row.template_id) << "'," << row.template_version << ","
        << "'" << SqlEscape(row.category) << "'," << row.priority << ","
        << "'" << SqlEscape(row.sender_name_snapshot) << "',"
        << "'" << SqlEscape(row.title_snapshot) << "',"
        << "'" << SqlEscape(row.body_snapshot) << "',"
        << "'" << SqlEscape(row.read_state) << "',";
    if (row.read_at > 0)
        sql << row.read_at;
    else
        sql << "NULL";
    sql << ",'" << SqlEscape(row.visible_state) << "'," << (row.has_attachment ? 1 : 0) << ","
        << "'" << SqlEscape(row.attachment_state) << "'," << (row.is_favorite ? 1 : 0) << ","
        << row.sent_at << "," << row.expire_at << ",";
    if (row.deleted_at > 0)
        sql << row.deleted_at;
    else
        sql << "NULL";
    sql << "," << row.row_version << "," << row.created_at << "," << row.updated_at << ")";

    if (!conn->update(sql.str())) {
        // 唯一键冲突视为重复投递
        MYSQL *raw = conn->raw();
        if (raw && mysql_errno(raw) == 1062) {
            *duplicate = true;
            return false;
        }
        return false;
    }
    MYSQL_RES *res = conn->query("SELECT LAST_INSERT_ID() AS id");
    if (!res)
        return false;
    MYSQL_ROW r = mysql_fetch_row(res);
    if (!r || !r[0]) {
        mysql_free_result(res);
        return false;
    }
    *mail_id = static_cast<uint64_t>(std::strtoull(r[0], nullptr, 10));
    mysql_free_result(res);
    return *mail_id > 0;
}

bool MailStore::InsertAttachment(Connection *conn, const mail::MailAttachmentRow &row) {
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "INSERT INTO mail_attachment (mail_id,slot_index,asset_type,asset_id,count,bind_type,"
        << "payload,claim_state,asset_transaction_id,claimed_at,created_at,updated_at) VALUES ("
        << row.mail_id << "," << row.slot_index << ",'" << SqlEscape(row.asset_type) << "',"
        << row.asset_id << "," << row.count << ",'" << SqlEscape(row.bind_type) << "','"
        << SqlEscape(row.payload) << "','" << SqlEscape(row.claim_state) << "','"
        << SqlEscape(row.asset_transaction_id) << "',";
    if (row.claimed_at > 0)
        sql << row.claimed_at;
    else
        sql << "NULL";
    sql << "," << row.created_at << "," << row.updated_at << ")";
    return conn->update(sql.str());
}

bool MailStore::CountActiveMails(uint64_t receiver_id, int *count) {
    if (!count || !EnsureTables())
        return false;
    auto conn = GetConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM mail_instance WHERE receiver_id=" << receiver_id
        << " AND visible_state='ACTIVE'";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    *count = row && row[0] ? std::atoi(row[0]) : 0;
    mysql_free_result(res);
    return true;
}

bool MailStore::CountFavorites(uint64_t receiver_id, int *count) {
    if (!count || !EnsureTables())
        return false;
    auto conn = GetConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM mail_instance WHERE receiver_id=" << receiver_id
        << " AND visible_state='ACTIVE' AND is_favorite=1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    *count = row && row[0] ? std::atoi(row[0]) : 0;
    mysql_free_result(res);
    return true;
}

bool MailStore::LoadMail(uint64_t mail_id, mail::MailInstanceRow *out) {
    if (!out || !EnsureTables())
        return false;
    auto conn = GetConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT " << kMailSelectCols << " FROM mail_instance WHERE mail_id=" << mail_id
        << " LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    const bool ok = FillMailRow(row, out);
    mysql_free_result(res);
    return ok;
}

bool MailStore::LoadMailForUpdate(Connection *conn, uint64_t mail_id, mail::MailInstanceRow *out) {
    if (!conn || !out)
        return false;
    std::ostringstream sql;
    sql << "SELECT " << kMailSelectCols << " FROM mail_instance WHERE mail_id=" << mail_id
        << " LIMIT 1 FOR UPDATE";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    const bool ok = FillMailRow(row, out);
    mysql_free_result(res);
    return ok;
}

bool MailStore::LoadAttachments(uint64_t mail_id, std::vector<mail::MailAttachmentRow> *out) {
    if (!out || !EnsureTables())
        return false;
    out->clear();
    auto conn = GetConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT attachment_id,mail_id,slot_index,asset_type,asset_id,count,bind_type,payload,"
        << "claim_state,asset_transaction_id,claimed_at FROM mail_attachment WHERE mail_id="
        << mail_id << " ORDER BY slot_index ASC";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        mail::MailAttachmentRow a;
        a.attachment_id = CellU64(row, 0);
        a.mail_id = CellU64(row, 1);
        a.slot_index = static_cast<int>(CellI64(row, 2));
        a.asset_type = CellStr(row, 3);
        a.asset_id = CellU64(row, 4);
        a.count = static_cast<uint32_t>(CellU64(row, 5));
        a.bind_type = CellStr(row, 6);
        a.payload = CellStr(row, 7);
        a.claim_state = CellStr(row, 8);
        a.asset_transaction_id = CellStr(row, 9);
        a.claimed_at = CellI64(row, 10);
        out->push_back(a);
    }
    mysql_free_result(res);
    return true;
}

bool MailStore::LoadAttachmentsForUpdate(Connection *conn, uint64_t mail_id,
                                         std::vector<mail::MailAttachmentRow> *out) {
    if (!conn || !out)
        return false;
    out->clear();
    std::ostringstream sql;
    sql << "SELECT attachment_id,mail_id,slot_index,asset_type,asset_id,count,bind_type,payload,"
        << "claim_state,asset_transaction_id,claimed_at FROM mail_attachment WHERE mail_id="
        << mail_id << " ORDER BY slot_index ASC FOR UPDATE";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        mail::MailAttachmentRow a;
        a.attachment_id = CellU64(row, 0);
        a.mail_id = CellU64(row, 1);
        a.slot_index = static_cast<int>(CellI64(row, 2));
        a.asset_type = CellStr(row, 3);
        a.asset_id = CellU64(row, 4);
        a.count = static_cast<uint32_t>(CellU64(row, 5));
        a.bind_type = CellStr(row, 6);
        a.payload = CellStr(row, 7);
        a.claim_state = CellStr(row, 8);
        a.asset_transaction_id = CellStr(row, 9);
        a.claimed_at = CellI64(row, 10);
        out->push_back(a);
    }
    mysql_free_result(res);
    return true;
}

bool MailStore::UpdateMailRow(Connection *conn, const mail::MailInstanceRow &row) {
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "UPDATE mail_instance SET read_state='" << SqlEscape(row.read_state) << "',";
    if (row.read_at > 0)
        sql << "read_at=" << row.read_at << ",";
    else
        sql << "read_at=NULL,";
    sql << "visible_state='" << SqlEscape(row.visible_state) << "',"
        << "attachment_state='" << SqlEscape(row.attachment_state) << "',"
        << "is_favorite=" << (row.is_favorite ? 1 : 0) << ",";
    if (row.deleted_at > 0)
        sql << "deleted_at=" << row.deleted_at << ",";
    else
        sql << "deleted_at=NULL,";
    sql << "row_version=" << row.row_version << ",updated_at=" << row.updated_at
        << " WHERE mail_id=" << row.mail_id;
    return conn->update(sql.str());
}

bool MailStore::UpdateAttachmentsClaimed(Connection *conn, uint64_t mail_id,
                                         const std::string &asset_tx_id, int64_t claimed_at) {
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "UPDATE mail_attachment SET claim_state='CLAIMED',asset_transaction_id='"
        << SqlEscape(asset_tx_id) << "',claimed_at=" << claimed_at << ",updated_at=" << claimed_at
        << " WHERE mail_id=" << mail_id;
    return conn->update(sql.str());
}

bool MailStore::UpdateAttachmentsClaimState(Connection *conn, uint64_t mail_id,
                                            const std::string &claim_state) {
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "UPDATE mail_attachment SET claim_state='" << SqlEscape(claim_state)
        << "',updated_at=UNIX_TIMESTAMP() WHERE mail_id=" << mail_id;
    return conn->update(sql.str());
}

bool MailStore::FindByBusinessKey(const std::string &source_system, const std::string &business_key,
                                  uint64_t receiver_id, mail::MailInstanceRow *out) {
    if (!out || !EnsureTables())
        return false;
    auto conn = GetConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT " << kMailSelectCols << " FROM mail_instance WHERE source_system='"
        << SqlEscape(source_system) << "' AND business_key='" << SqlEscape(business_key)
        << "' AND receiver_id=" << receiver_id << " LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    const bool ok = FillMailRow(row, out);
    mysql_free_result(res);
    return ok;
}

bool MailStore::FindOpByIdempotency(const std::string &key, MailOpLogRow *out) {
    if (!out || !EnsureTables())
        return false;
    auto conn = GetConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT operation_id,mail_id,actor_id,operation_type,idempotency_key,before_state,"
        << "after_state,result_code,trace_id,created_at FROM mail_operation_log WHERE "
           "idempotency_key='"
        << SqlEscape(key) << "' LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return false;
    }
    out->operation_id = CellU64(row, 0);
    out->mail_id = CellU64(row, 1);
    out->actor_id = CellU64(row, 2);
    out->operation_type = CellStr(row, 3);
    out->idempotency_key = CellStr(row, 4);
    out->before_state = CellStr(row, 5);
    out->after_state = CellStr(row, 6);
    out->result_code = CellStr(row, 7);
    out->trace_id = CellStr(row, 8);
    out->created_at = CellI64(row, 9);
    mysql_free_result(res);
    return true;
}

bool MailStore::InsertOpLog(Connection *conn, const MailOpLogRow &row) {
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "INSERT INTO mail_operation_log (mail_id,actor_id,operation_type,idempotency_key,"
        << "before_state,after_state,result_code,trace_id,created_at) VALUES (" << row.mail_id
        << "," << row.actor_id << ",'" << SqlEscape(row.operation_type) << "','"
        << SqlEscape(row.idempotency_key) << "','" << SqlEscape(row.before_state) << "','"
        << SqlEscape(row.after_state) << "','" << SqlEscape(row.result_code) << "','"
        << SqlEscape(row.trace_id) << "'," << row.created_at << ")";
    return conn->update(sql.str());
}

bool MailStore::Summarize(uint64_t receiver_id, int64_t now_utc, int64_t /*mailbox_version*/,
                          int *unread_system, int *unread_activity, int *unread_social,
                          int *unread_trade, int *unclaimed_attach, int *expiring_soon,
                          int *current_count) {
    if (!EnsureTables())
        return false;
    auto conn = GetConnection();
    if (!conn)
        return false;
    *unread_system = *unread_activity = *unread_social = *unread_trade = 0;
    *unclaimed_attach = *expiring_soon = *current_count = 0;

    {
        std::ostringstream sql;
        sql << "SELECT category, COUNT(*) FROM mail_instance WHERE receiver_id=" << receiver_id
            << " AND visible_state='ACTIVE' AND read_state='UNREAD' GROUP BY category";
        MYSQL_RES *res = conn->query(sql.str());
        if (!res)
            return false;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            const std::string cat = CellStr(row, 0);
            const int n = row[1] ? std::atoi(row[1]) : 0;
            if (cat == "ACTIVITY")
                *unread_activity = n;
            else if (cat == "SOCIAL")
                *unread_social = n;
            else if (cat == "TRADE")
                *unread_trade = n;
            else
                *unread_system = n;
        }
        mysql_free_result(res);
    }
    {
        std::ostringstream sql;
        sql << "SELECT COUNT(*) FROM mail_instance WHERE receiver_id=" << receiver_id
            << " AND visible_state='ACTIVE' AND attachment_state='UNCLAIMED'";
        MYSQL_RES *res = conn->query(sql.str());
        if (!res)
            return false;
        MYSQL_ROW row = mysql_fetch_row(res);
        *unclaimed_attach = row && row[0] ? std::atoi(row[0]) : 0;
        mysql_free_result(res);
    }
    {
        const int64_t soon = now_utc + 72 * 3600;
        std::ostringstream sql;
        sql << "SELECT COUNT(*) FROM mail_instance WHERE receiver_id=" << receiver_id
            << " AND visible_state='ACTIVE' AND expire_at>" << now_utc << " AND expire_at<="
            << soon;
        MYSQL_RES *res = conn->query(sql.str());
        if (!res)
            return false;
        MYSQL_ROW row = mysql_fetch_row(res);
        *expiring_soon = row && row[0] ? std::atoi(row[0]) : 0;
        mysql_free_result(res);
    }
    return CountActiveMails(receiver_id, current_count);
}

namespace {

bool DecodeCursor(const std::string &cursor, int *priority, int *unclaimed, int *unread,
                  int64_t *sent_at, uint64_t *mail_id) {
    if (cursor.empty())
        return false;
    // format: p|u|r|s|id
    int p = 0, u = 0, r = 0;
    long long s = 0;
    unsigned long long id = 0;
    if (std::sscanf(cursor.c_str(), "%d|%d|%d|%lld|%llu", &p, &u, &r, &s, &id) != 5)
        return false;
    *priority = p;
    *unclaimed = u;
    *unread = r;
    *sent_at = static_cast<int64_t>(s);
    *mail_id = static_cast<uint64_t>(id);
    return true;
}

std::string EncodeCursor(int priority, int unclaimed, int unread, int64_t sent_at,
                         uint64_t mail_id) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%d|%d|%d|%lld|%llu", priority, unclaimed, unread,
                  static_cast<long long>(sent_at), static_cast<unsigned long long>(mail_id));
    return buf;
}

}  // namespace

bool MailStore::ListMails(uint64_t receiver_id, const MailListFilter &filter,
                          std::vector<MailListItem> *out, std::string *next_cursor) {
    if (!out || !next_cursor || !EnsureTables())
        return false;
    out->clear();
    *next_cursor = "";
    auto conn = GetConnection();
    if (!conn)
        return false;

    int limit = filter.limit > 0 ? filter.limit : 20;
    if (limit > 100)
        limit = 100;

    std::ostringstream sql;
    sql << "SELECT " << kMailSelectCols << " FROM mail_instance WHERE receiver_id=" << receiver_id
        << " AND visible_state='ACTIVE'";
    if (!filter.category.empty())
        sql << " AND category='" << SqlEscape(filter.category) << "'";
    if (filter.only_unread)
        sql << " AND read_state='UNREAD'";
    if (filter.only_has_attachment)
        sql << " AND has_attachment=1";
    if (filter.only_unclaimed)
        sql << " AND attachment_state='UNCLAIMED'";
    if (filter.only_favorite)
        sql << " AND is_favorite=1";
    if (filter.only_expiring_soon) {
        // 调用方应传入 now；此处用 UNIX_TIMESTAMP 近似
        sql << " AND expire_at>UNIX_TIMESTAMP() AND expire_at<=UNIX_TIMESTAMP()+259200";
    }
    if (!filter.keyword.empty())
        sql << " AND (title_snapshot LIKE '%" << SqlEscape(filter.keyword)
            << "%' OR body_snapshot LIKE '%" << SqlEscape(filter.keyword) << "%')";

    int cp = 0, cu = 0, cr = 0;
    int64_t cs = 0;
    uint64_t cid = 0;
    if (DecodeCursor(filter.cursor, &cp, &cu, &cr, &cs, &cid)) {
        // priority DESC, unclaimed DESC, unread DESC, sent_at DESC, mail_id DESC
        sql << " AND (priority < " << cp << " OR (priority=" << cp << " AND "
            << "(CASE WHEN attachment_state='UNCLAIMED' THEN 1 ELSE 0 END) < " << cu
            << ") OR (priority=" << cp
            << " AND (CASE WHEN attachment_state='UNCLAIMED' THEN 1 ELSE 0 END)=" << cu
            << " AND (CASE WHEN read_state='UNREAD' THEN 1 ELSE 0 END) < " << cr << ") OR (priority="
            << cp << " AND (CASE WHEN attachment_state='UNCLAIMED' THEN 1 ELSE 0 END)=" << cu
            << " AND (CASE WHEN read_state='UNREAD' THEN 1 ELSE 0 END)=" << cr << " AND (sent_at < "
            << cs << " OR (sent_at=" << cs << " AND mail_id < " << cid << "))))";
    }

    sql << " ORDER BY priority DESC, "
        << "(CASE WHEN attachment_state='UNCLAIMED' THEN 1 ELSE 0 END) DESC, "
        << "(CASE WHEN read_state='UNREAD' THEN 1 ELSE 0 END) DESC, "
        << "sent_at DESC, mail_id DESC LIMIT " << (limit + 1);

    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        MailListItem item;
        if (!FillMailRow(row, &item.row))
            continue;
        out->push_back(item);
    }
    mysql_free_result(res);

    if (static_cast<int>(out->size()) > limit) {
        out->pop_back();
        const auto &last = out->back().row;
        const int u = last.attachment_state == "UNCLAIMED" ? 1 : 0;
        const int r = last.read_state == "UNREAD" ? 1 : 0;
        *next_cursor = EncodeCursor(last.priority, u, r, last.sent_at, last.mail_id);
    }
    return true;
}

bool MailStore::SelectAutoCleanupCandidates(uint64_t receiver_id, int64_t now_utc, int protect_hours,
                                            int limit, std::vector<uint64_t> *mail_ids) {
    if (!mail_ids || !EnsureTables())
        return false;
    mail_ids->clear();
    auto conn = GetConnection();
    if (!conn)
        return false;
    const int64_t protect_before = now_utc - static_cast<int64_t>(protect_hours) * 3600;
    // 顺序：已读无附件 > 附件已领取 > 已读活动 > 其他可清理
    std::ostringstream sql;
    sql << "SELECT mail_id FROM mail_instance WHERE receiver_id=" << receiver_id
        << " AND visible_state='ACTIVE' AND is_favorite=0"
        << " AND attachment_state NOT IN ('UNCLAIMED','CLAIMING')"
        << " AND sent_at<=" << protect_before
        << " ORDER BY "
        << "CASE "
        << "WHEN read_state='READ' AND has_attachment=0 THEN 0 "
        << "WHEN attachment_state='CLAIMED' THEN 1 "
        << "WHEN read_state='READ' AND category='ACTIVITY' THEN 2 "
        << "ELSE 3 END ASC, sent_at ASC LIMIT " << limit;
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0])
            mail_ids->push_back(static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10)));
    }
    mysql_free_result(res);
    return true;
}

bool MailStore::MarkExpiredBatch(int64_t now_utc, int limit, int *affected) {
    if (!affected || !EnsureTables())
        return false;
    *affected = 0;
    auto conn = GetConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "UPDATE mail_instance SET visible_state='EXPIRED',updated_at=" << now_utc
        << ",row_version=row_version+1 WHERE visible_state='ACTIVE' AND expire_at>0 AND expire_at<="
        << now_utc << " LIMIT " << limit;
    if (!conn->update(sql.str()))
        return false;
    *affected = static_cast<int>(mysql_affected_rows(conn->raw()));
    return true;
}
