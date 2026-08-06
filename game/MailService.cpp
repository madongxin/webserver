#include "MailService.h"

#include "Connection.h"
#include "Logging.h"
#include "MailConfig.h"
#include "MailStore.h"
#include "PlayerItemStore.h"
#include "GameLogic.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace {

constexpr int64_t kInventorySoftCap = 999999;

}  // namespace

MailService &MailService::Instance() {
    static MailService g;
    return g;
}

int64_t MailService::NowUtc() const {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool MailService::Init() {
    MailConfig::Instance().LoadFromConfig();
    if (!MailStore::Instance().EnsureTables()) {
        LOG_WARN << "MailService: EnsureTables failed";
        ready_ = false;
        return false;
    }
    ready_ = true;
    LOG_INFO << "MailService: ready";
    return true;
}

int64_t MailService::MailboxVersion(uint64_t player_id) const {
    std::lock_guard<std::mutex> lk(ver_mu_);
    auto it = mailbox_version_.find(player_id);
    return it == mailbox_version_.end() ? 1 : it->second;
}

void MailService::BumpVersion(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(ver_mu_);
    auto it = mailbox_version_.find(player_id);
    const int64_t cur = it == mailbox_version_.end() ? 1 : it->second;
    mailbox_version_[player_id] = cur + 1;
}

bool MailService::TouchExpireIfNeeded(mail::MailInstanceRow *row, int64_t now) {
    if (!row || row->visible_state != "ACTIVE")
        return false;
    if (!mail::IsExpiredAt(row->expire_at, now))
        return false;
    if (!mail::CanTransitVisible(mail::VisibleState::kActive, mail::VisibleState::kExpired))
        return false;
    auto conn = MailStore::Instance().GetConnection();
    if (!conn)
        return false;
    row->visible_state = "EXPIRED";
    row->updated_at = now;
    row->row_version += 1;
    return MailStore::Instance().UpdateMailRow(conn.get(), *row);
}

bool MailService::SoftDeleteMail(uint64_t mail_id, uint64_t /*actor_id*/, int64_t now) {
    mail::MailInstanceRow row;
    if (!MailStore::Instance().LoadMail(mail_id, &row))
        return false;
    auto conn = MailStore::Instance().GetConnection();
    if (!conn)
        return false;
    row.visible_state = "SOFT_DELETED";
    row.deleted_at = now;
    row.updated_at = now;
    row.row_version += 1;
    return MailStore::Instance().UpdateMailRow(conn.get(), row);
}

bool MailService::EnsureCapacityForDeliver(uint64_t receiver_id, std::string *error_code,
                                           std::string *message) {
    const auto &cfg = MailConfig::Instance().Values();
    int count = 0;
    if (!MailStore::Instance().CountActiveMails(receiver_id, &count)) {
        *error_code = mail::err::kInternal;
        *message = "count mailbox failed";
        return false;
    }
    if (count < cfg.mailbox_overflow)
        return true;

    // 尝试自动清理到 overflow 以下，优先清到 capacity
    const int need = count - cfg.mailbox_capacity + 1;
    std::vector<uint64_t> victims;
    if (!MailStore::Instance().SelectAutoCleanupCandidates(receiver_id, NowUtc(),
                                                           cfg.protect_hours, need + 5, &victims)) {
        *error_code = mail::err::kInternal;
        *message = "cleanup query failed";
        return false;
    }
    const int64_t now = NowUtc();
    for (uint64_t id : victims) {
        SoftDeleteMail(id, 0, now);
        if (!MailStore::Instance().CountActiveMails(receiver_id, &count))
            break;
        if (count < cfg.mailbox_overflow)
            break;
    }
    if (!MailStore::Instance().CountActiveMails(receiver_id, &count)) {
        *error_code = mail::err::kInternal;
        *message = "recount failed";
        return false;
    }
    if (count >= cfg.mailbox_overflow) {
        *error_code = mail::err::kMailboxFull;
        *message = "mailbox full even after cleanup";
        return false;
    }
    return true;
}

bool MailService::Deliver(const mail::DeliverRequest &req, uint64_t *mail_id,
                          std::string *error_code, std::string *message) {
    if (!ready_ && !Init()) {
        *error_code = mail::err::kInternal;
        *message = "mail service not ready";
        return false;
    }
    if (!mail_id || !error_code || !message) {
        return false;
    }
    if (req.source_system.empty() || req.business_key.empty() || req.receiver_id == 0 ||
        req.title.empty()) {
        *error_code = mail::err::kInvalidArgument;
        *message = "source_system, business_key, receiver_id, title required";
        return false;
    }
    if (req.receiver_type != "ROLE" && !req.receiver_type.empty()) {
        // 首期仅 ROLE；ACCOUNT 字段预留
        if (req.receiver_type != "ACCOUNT") {
            *error_code = mail::err::kInvalidArgument;
            *message = "receiver_type invalid";
            return false;
        }
        *error_code = mail::err::kInvalidArgument;
        *message = "ACCOUNT scope not implemented in phase1";
        return false;
    }
    mail::Category cat;
    if (!mail::ParseCategory(req.category, &cat)) {
        *error_code = mail::err::kInvalidArgument;
        *message = "invalid category";
        return false;
    }
    for (const auto &a : req.attachments) {
        if (a.asset_type != "ITEM" || a.asset_id == 0 || a.count == 0) {
            *error_code = mail::err::kInvalidAttachment;
            *message = "only ITEM attachments supported";
            return false;
        }
    }

    mail::MailInstanceRow existing;
    if (MailStore::Instance().FindByBusinessKey(req.source_system, req.business_key,
                                                req.receiver_id, &existing)) {
        // 幂等命中：内容不同则冲突
        if (existing.title_snapshot != req.title || existing.body_snapshot != req.body ||
            existing.category != mail::ToString(cat)) {
            *error_code = mail::err::kIdempotencyConflict;
            *message = "same business_key with different content";
            LOG_ERROR << "MailDeliver IDEMPOTENCY_CONFLICT source=" << req.source_system
                      << " key=" << req.business_key << " receiver=" << req.receiver_id;
            return false;
        }
        *mail_id = existing.mail_id;
        *error_code = mail::err::kOk;
        *message = "idempotent hit";
        return true;
    }

    if (!EnsureCapacityForDeliver(req.receiver_id, error_code, message))
        return false;

    const auto &cfg = MailConfig::Instance().Values();
    const int64_t now = NowUtc();
    mail::MailInstanceRow row;
    row.owner_scope = "ROLE";
    row.receiver_id = req.receiver_id;
    row.sender_type = "SYSTEM";
    row.sender_id = 0;
    row.source_system = req.source_system;
    row.business_key = req.business_key;
    row.template_id = req.template_id;
    row.template_version = req.template_version > 0 ? req.template_version : 1;
    row.category = mail::ToString(cat);
    row.priority = req.priority;
    row.sender_name_snapshot = req.sender_name.empty() ? "System" : req.sender_name;
    row.title_snapshot = req.title;
    row.body_snapshot = req.body;
    row.read_state = "UNREAD";
    row.visible_state = "ACTIVE";
    row.has_attachment = !req.attachments.empty();
    row.attachment_state = req.attachments.empty() ? "NONE" : "UNCLAIMED";
    row.sent_at = req.send_at > 0 ? req.send_at : now;
    row.expire_at =
        req.expire_at > 0 ? req.expire_at : now + static_cast<int64_t>(cfg.default_expire_days) * 86400;
    row.row_version = 1;
    row.created_at = now;
    row.updated_at = now;

    auto conn = MailStore::Instance().GetConnection();
    if (!conn) {
        *error_code = mail::err::kInternal;
        *message = "no db connection";
        return false;
    }
    if (!MailStore::Instance().Begin(conn.get())) {
        *error_code = mail::err::kInternal;
        *message = "begin failed";
        return false;
    }
    bool dup = false;
    uint64_t id = 0;
    if (!MailStore::Instance().InsertMail(conn.get(), row, &id, &dup)) {
        MailStore::Instance().Rollback(conn.get());
        if (dup) {
            if (MailStore::Instance().FindByBusinessKey(req.source_system, req.business_key,
                                                        req.receiver_id, &existing)) {
                *mail_id = existing.mail_id;
                *error_code = mail::err::kOk;
                *message = "idempotent hit";
                return true;
            }
            *error_code = mail::err::kIdempotencyConflict;
            *message = "duplicate key";
            return false;
        }
        *error_code = mail::err::kInternal;
        *message = "insert mail failed";
        return false;
    }
    int slot = 0;
    for (const auto &a : req.attachments) {
        mail::MailAttachmentRow ar;
        ar.mail_id = id;
        ar.slot_index = slot++;
        ar.asset_type = a.asset_type;
        ar.asset_id = a.asset_id;
        ar.count = a.count;
        ar.bind_type = a.bind_type.empty() ? "NONE" : a.bind_type;
        ar.payload = a.payload;
        ar.claim_state = "UNCLAIMED";
        ar.created_at = now;
        ar.updated_at = now;
        if (!MailStore::Instance().InsertAttachment(conn.get(), ar)) {
            MailStore::Instance().Rollback(conn.get());
            *error_code = mail::err::kInternal;
            *message = "insert attachment failed";
            return false;
        }
    }
    MailOpLogRow op;
    op.mail_id = id;
    op.actor_id = 0;
    op.operation_type = "DELIVER";
    op.idempotency_key = "deliver:" + req.source_system + ":" + req.business_key + ":" +
                         std::to_string(req.receiver_id);
    op.before_state = "{}";
    op.after_state = "{\"mail_id\":" + std::to_string(id) + "}";
    op.result_code = mail::err::kOk;
    op.trace_id = req.trace_id;
    op.created_at = now;
    if (!MailStore::Instance().InsertOpLog(conn.get(), op)) {
        // 投递日志失败不阻断（唯一键冲突视为并发投递）
        LOG_WARN << "MailDeliver: op log insert failed mail_id=" << id;
    }
    if (!MailStore::Instance().Commit(conn.get())) {
        MailStore::Instance().Rollback(conn.get());
        *error_code = mail::err::kInternal;
        *message = "commit failed";
        return false;
    }
    *mail_id = id;
    *error_code = mail::err::kOk;
    *message = "delivered";
    BumpVersion(req.receiver_id);
    return true;
}

void MailService::FillMailBrief(const mail::MailInstanceRow &row, game::MailBrief *out) {
    out->set_mail_id(row.mail_id);
    out->set_category(row.category);
    out->set_priority(row.priority);
    out->set_title(row.title_snapshot);
    out->set_sender_name(row.sender_name_snapshot);
    out->set_read_state(row.read_state);
    out->set_visible_state(row.visible_state);
    out->set_attachment_state(row.attachment_state);
    out->set_has_attachment(row.has_attachment);
    out->set_is_favorite(row.is_favorite);
    out->set_sent_at(row.sent_at);
    out->set_expire_at(row.expire_at);
    out->set_row_version(row.row_version);
}

void MailService::FillMailDetail(const mail::MailInstanceRow &row,
                                 const std::vector<mail::MailAttachmentRow> &atts,
                                 game::MailDetail *out) {
    FillMailBrief(row, out->mutable_brief());
    out->set_body(row.body_snapshot);
    out->clear_attachments();
    for (const auto &a : atts) {
        auto *d = out->add_attachments();
        d->set_slot_index(static_cast<uint32_t>(a.slot_index));
        d->set_asset_type(a.asset_type);
        d->set_asset_id(a.asset_id);
        d->set_count(a.count);
        d->set_bind_type(a.bind_type);
        d->set_payload(a.payload);
        d->set_claim_state(a.claim_state);
    }
    out->clear_allowed_actions();
    if (row.visible_state == "ACTIVE") {
        if (row.read_state == "UNREAD")
            out->add_allowed_actions("READ");
        if (row.attachment_state == "UNCLAIMED")
            out->add_allowed_actions("CLAIM");
        out->add_allowed_actions(row.is_favorite ? "UNFAVORITE" : "FAVORITE");
        if (row.attachment_state != "UNCLAIMED" && row.attachment_state != "CLAIMING")
            out->add_allowed_actions("DELETE");
    }
}

bool MailService::HandleMailboxSummary(const game::MailboxSummaryReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mailbox_summary();
    const int64_t now = NowUtc();
    body->set_server_time_utc(now);
    if (!ready_ && !Init()) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("mail not ready");
        rsp->set_message(body->message());
        return false;
    }
    ScanExpire(50);
    int us = 0, ua = 0, uso = 0, ut = 0, unc = 0, exp = 0, cur = 0;
    if (!MailStore::Instance().Summarize(req.player_id(), now, MailboxVersion(req.player_id()), &us,
                                         &ua, &uso, &ut, &unc, &exp, &cur)) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("summarize failed");
        rsp->set_message(body->message());
        return false;
    }
    const auto &cfg = MailConfig::Instance().Values();
    body->set_ok(true);
    body->set_error_code(mail::err::kOk);
    body->set_message("ok");
    body->set_unread_system(static_cast<uint32_t>(us));
    body->set_unread_activity(static_cast<uint32_t>(ua));
    body->set_unread_social(static_cast<uint32_t>(uso));
    body->set_unread_trade(static_cast<uint32_t>(ut));
    body->set_unclaimed_attachment(static_cast<uint32_t>(unc));
    body->set_expiring_soon(static_cast<uint32_t>(exp));
    body->set_current_count(static_cast<uint32_t>(cur));
    body->set_max_capacity(static_cast<uint32_t>(cfg.mailbox_capacity));
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool MailService::HandleMailList(const game::MailListReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_list();
    const int64_t now = NowUtc();
    body->set_server_time_utc(now);
    if (!ready_ && !Init()) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("mail not ready");
        rsp->set_message(body->message());
        return false;
    }
    ScanExpire(50);
    MailListFilter f;
    f.category = req.category();
    f.only_unread = req.unread();
    f.only_has_attachment = req.has_attachment();
    f.only_unclaimed = req.unclaimed();
    f.only_expiring_soon = req.expiring_soon();
    f.only_favorite = req.favorite();
    f.keyword = req.keyword();
    f.cursor = req.cursor();
    f.limit = req.limit() > 0 ? req.limit() : MailConfig::Instance().Values().page_size_default;
    std::vector<MailListItem> items;
    std::string next;
    if (!MailStore::Instance().ListMails(req.player_id(), f, &items, &next)) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("list failed");
        rsp->set_message(body->message());
        return false;
    }
    for (const auto &it : items) {
        mail::MailInstanceRow row = it.row;
        TouchExpireIfNeeded(&row, now);
        if (row.visible_state != "ACTIVE")
            continue;
        FillMailBrief(row, body->add_mails());
    }
    body->set_next_cursor(next);
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    body->set_ok(true);
    body->set_error_code(mail::err::kOk);
    body->set_message("ok");
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool MailService::HandleMailGet(const game::MailGetReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_get();
    const int64_t now = NowUtc();
    body->set_server_time_utc(now);
    mail::MailInstanceRow row;
    if (!MailStore::Instance().LoadMail(req.mail_id(), &row)) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kMailNotFound);
        body->set_message("mail not found");
        rsp->set_message(body->message());
        return false;
    }
    if (row.receiver_id != req.player_id()) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kPermissionDenied);
        body->set_message("permission denied");
        rsp->set_message(body->message());
        return false;
    }
    TouchExpireIfNeeded(&row, now);
    std::vector<mail::MailAttachmentRow> atts;
    MailStore::Instance().LoadAttachments(row.mail_id, &atts);
    FillMailDetail(row, atts, body->mutable_mail());
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    body->set_ok(true);
    body->set_error_code(mail::err::kOk);
    body->set_message("ok");
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool MailService::HandleMailRead(const game::MailReadReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_read();
    const int64_t now = NowUtc();
    body->set_server_time_utc(now);
    if (req.idempotency_key().empty()) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInvalidArgument);
        body->set_message("idempotency_key required");
        rsp->set_message(body->message());
        return false;
    }
    MailOpLogRow existed;
    if (MailStore::Instance().FindOpByIdempotency(req.idempotency_key(), &existed)) {
        rsp->set_ok(existed.result_code == mail::err::kOk);
        body->set_ok(rsp->ok());
        body->set_error_code(existed.result_code);
        body->set_message("idempotent");
        body->set_mailbox_version(MailboxVersion(req.player_id()));
        rsp->set_message(body->message());
        return rsp->ok();
    }
    mail::MailInstanceRow row;
    if (!MailStore::Instance().LoadMail(req.mail_id(), &row) ||
        row.receiver_id != req.player_id()) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(row.mail_id ? mail::err::kPermissionDenied : mail::err::kMailNotFound);
        body->set_message("not found or denied");
        rsp->set_message(body->message());
        return false;
    }
    TouchExpireIfNeeded(&row, now);
    if (row.visible_state == "EXPIRED") {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kMailExpired);
        body->set_message("expired");
        rsp->set_message(body->message());
        return false;
    }
    if (row.visible_state == "REVOKED") {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kMailRevoked);
        body->set_message("revoked");
        rsp->set_message(body->message());
        return false;
    }
    auto conn = MailStore::Instance().GetConnection();
    if (!conn) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("db");
        rsp->set_message(body->message());
        return false;
    }
    if (row.read_state != "READ") {
        row.read_state = "READ";
        row.read_at = now;
        row.updated_at = now;
        row.row_version += 1;
        if (!MailStore::Instance().UpdateMailRow(conn.get(), row)) {
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("update failed");
            rsp->set_message(body->message());
            return false;
        }
    }
    MailOpLogRow op;
    op.mail_id = row.mail_id;
    op.actor_id = req.player_id();
    op.operation_type = "READ";
    op.idempotency_key = req.idempotency_key();
    op.before_state = "{\"read\":\"UNREAD\"}";
    op.after_state = "{\"read\":\"READ\"}";
    op.result_code = mail::err::kOk;
    op.created_at = now;
    MailStore::Instance().InsertOpLog(conn.get(), op);
    BumpVersion(req.player_id());
    body->set_ok(true);
    body->set_error_code(mail::err::kOk);
    body->set_message("ok");
    body->set_read_at(row.read_at > 0 ? row.read_at : now);
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool MailService::ClaimOne(uint64_t player_id, uint64_t mail_id, const std::string &idempotency_key,
                           const std::string &trace_id, game::MailClaimResult *result) {
    result->set_mail_id(mail_id);
    result->set_ok(false);
    if (idempotency_key.empty()) {
        result->set_error_code(mail::err::kInvalidArgument);
        result->set_message("idempotency_key required");
        return false;
    }
    MailOpLogRow existed;
    if (MailStore::Instance().FindOpByIdempotency(idempotency_key, &existed)) {
        result->set_ok(existed.result_code == mail::err::kOk ||
                       existed.result_code == mail::err::kAlreadyClaimed);
        result->set_error_code(existed.result_code);
        result->set_message("idempotent");
        result->set_attachment_state(existed.result_code == mail::err::kOk ? "CLAIMED" : "");
        return result->ok();
    }

    auto conn = MailStore::Instance().GetConnection();
    if (!conn) {
        result->set_error_code(mail::err::kInternal);
        result->set_message("db");
        return false;
    }
    const int64_t now = NowUtc();
    if (!MailStore::Instance().Begin(conn.get())) {
        result->set_error_code(mail::err::kInternal);
        result->set_message("begin");
        return false;
    }
    mail::MailInstanceRow row;
    if (!MailStore::Instance().LoadMailForUpdate(conn.get(), mail_id, &row)) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kMailNotFound);
        result->set_message("not found");
        return false;
    }
    if (row.receiver_id != player_id) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kPermissionDenied);
        result->set_message("denied");
        return false;
    }
    if (mail::IsExpiredAt(row.expire_at, now) || row.visible_state == "EXPIRED") {
        row.visible_state = "EXPIRED";
        row.updated_at = now;
        row.row_version += 1;
        MailStore::Instance().UpdateMailRow(conn.get(), row);
        MailStore::Instance().Commit(conn.get());
        result->set_error_code(mail::err::kMailExpired);
        result->set_message("expired");
        return false;
    }
    if (row.visible_state == "REVOKED") {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kMailRevoked);
        result->set_message("revoked");
        return false;
    }
    if (row.visible_state != "ACTIVE") {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInvalidState);
        result->set_message("not active");
        return false;
    }
    if (row.attachment_state == "CLAIMED") {
        MailOpLogRow op;
        op.mail_id = mail_id;
        op.actor_id = player_id;
        op.operation_type = "CLAIM";
        op.idempotency_key = idempotency_key;
        op.before_state = "{\"attachment\":\"CLAIMED\"}";
        op.after_state = "{\"attachment\":\"CLAIMED\"}";
        op.result_code = mail::err::kAlreadyClaimed;
        op.trace_id = trace_id;
        op.created_at = now;
        MailStore::Instance().InsertOpLog(conn.get(), op);
        MailStore::Instance().Commit(conn.get());
        result->set_ok(true);
        result->set_error_code(mail::err::kAlreadyClaimed);
        result->set_message("already claimed");
        result->set_attachment_state("CLAIMED");
        return true;
    }
    if (row.attachment_state == "CLAIMING") {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kClaimInProgress);
        result->set_message("claiming");
        return false;
    }
    if (row.attachment_state != "UNCLAIMED") {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInvalidAttachment);
        result->set_message("no claimable attachment");
        return false;
    }
    if (!mail::CanTransitAttachment(mail::AttachmentState::kUnclaimed,
                                    mail::AttachmentState::kClaiming)) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInvalidState);
        result->set_message("illegal transition");
        return false;
    }

    row.attachment_state = "CLAIMING";
    row.updated_at = now;
    row.row_version += 1;
    if (!MailStore::Instance().UpdateMailRow(conn.get(), row) ||
        !MailStore::Instance().UpdateAttachmentsClaimState(conn.get(), mail_id, "CLAIMING")) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInternal);
        result->set_message("mark claiming failed");
        return false;
    }

    std::vector<mail::MailAttachmentRow> atts;
    if (!MailStore::Instance().LoadAttachmentsForUpdate(conn.get(), mail_id, &atts) || atts.empty()) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInvalidAttachment);
        result->set_message("attachments missing");
        return false;
    }

    // dry-run：首期仅校验 ITEM 与软上限
    for (const auto &a : atts) {
        if (a.asset_type != "ITEM" || a.asset_id == 0 || a.count == 0) {
            MailStore::Instance().Rollback(conn.get());
            result->set_error_code(mail::err::kInvalidAttachment);
            result->set_message("invalid asset");
            return false;
        }
        if (a.asset_id > 0xFFFFFFFFu) {
            MailStore::Instance().Rollback(conn.get());
            result->set_error_code(mail::err::kInvalidAttachment);
            result->set_message("asset_id too large");
            return false;
        }
        const uint32_t cur =
            GameLogic::Instance().GetItemCount(player_id, static_cast<uint32_t>(a.asset_id));
        if (static_cast<int64_t>(cur) + a.count > kInventorySoftCap) {
            MailStore::Instance().Rollback(conn.get());
            result->set_error_code(mail::err::kInventoryFull);
            result->set_message("inventory soft cap");
            return false;
        }
    }

    std::ostringstream tx;
    tx << "mail:" << mail_id << ":" << now;
    const std::string asset_tx = tx.str();
    for (const auto &a : atts) {
        uint64_t instance_id = 0;
        std::ostringstream extra;
        extra << "{\"source\":\"mail_claim\",\"mail_id\":" << mail_id
              << ",\"tx\":\"" << asset_tx << "\"}";
        if (!PlayerItemStore::Instance().InsertOnConnection(conn.get(), player_id, a.asset_id,
                                                            a.count, 0, extra.str(), &instance_id)) {
            MailStore::Instance().Rollback(conn.get());
            result->set_error_code(mail::err::kAssetTransactionFailed);
            result->set_message("asset insert failed");
            return false;
        }
    }

    row.attachment_state = "CLAIMED";
    if (row.read_state != "READ") {
        row.read_state = "READ";
        row.read_at = now;
    }
    row.updated_at = now;
    row.row_version += 1;
    if (!MailStore::Instance().UpdateMailRow(conn.get(), row) ||
        !MailStore::Instance().UpdateAttachmentsClaimed(conn.get(), mail_id, asset_tx, now)) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInternal);
        result->set_message("finalize claim failed");
        return false;
    }

    MailOpLogRow op;
    op.mail_id = mail_id;
    op.actor_id = player_id;
    op.operation_type = "CLAIM";
    op.idempotency_key = idempotency_key;
    op.before_state = "{\"attachment\":\"UNCLAIMED\"}";
    op.after_state = "{\"attachment\":\"CLAIMED\",\"tx\":\"" + asset_tx + "\"}";
    op.result_code = mail::err::kOk;
    op.trace_id = trace_id;
    op.created_at = now;
    if (!MailStore::Instance().InsertOpLog(conn.get(), op)) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInternal);
        result->set_message("op log failed");
        return false;
    }
    if (!MailStore::Instance().Commit(conn.get())) {
        MailStore::Instance().Rollback(conn.get());
        result->set_error_code(mail::err::kInternal);
        result->set_message("commit failed");
        return false;
    }
    // 事务成功后再改内存背包，避免回滚导致“库无货、内存有货”
    for (const auto &a : atts)
        GameLogic::Instance().ApplyItemReward(player_id, static_cast<uint32_t>(a.asset_id), a.count);
    BumpVersion(player_id);
    result->set_ok(true);
    result->set_error_code(mail::err::kOk);
    result->set_message("claimed");
    result->set_attachment_state("CLAIMED");
    return true;
}

bool MailService::HandleMailClaim(const game::MailClaimReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_claim();
    body->set_server_time_utc(NowUtc());
    if (!ready_ && !Init()) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("mail not ready");
        rsp->set_message(body->message());
        return false;
    }
    game::MailClaimResult result;
    const bool ok =
        ClaimOne(req.player_id(), req.mail_id(), req.idempotency_key(), req.trace_id(), &result);
    *body->mutable_result() = result;
    body->set_ok(ok);
    body->set_error_code(result.error_code());
    body->set_message(result.message());
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    rsp->set_ok(ok);
    rsp->set_message(result.message());
    return ok;
}

bool MailService::HandleMailBatchClaim(const game::MailBatchClaimReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_batch_claim();
    body->set_server_time_utc(NowUtc());
    const auto &cfg = MailConfig::Instance().Values();
    if (req.mail_ids_size() == 0 || req.mail_ids_size() > cfg.batch_op_max) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInvalidArgument);
        body->set_message("mail_ids size invalid");
        rsp->set_message(body->message());
        return false;
    }
    // 按过期时间升序
    std::vector<std::pair<int64_t, uint64_t>> order;
    for (int i = 0; i < req.mail_ids_size(); ++i) {
        mail::MailInstanceRow row;
        if (!MailStore::Instance().LoadMail(req.mail_ids(i), &row) ||
            row.receiver_id != req.player_id())
            continue;
        // 选择箱/高价值确认：payload 含 need_confirm 不进批量
        std::vector<mail::MailAttachmentRow> atts;
        MailStore::Instance().LoadAttachments(row.mail_id, &atts);
        bool skip = false;
        for (const auto &a : atts) {
            if (a.payload.find("need_confirm") != std::string::npos ||
                a.payload.find("COD") != std::string::npos) {
                skip = true;
                break;
            }
        }
        if (skip)
            continue;
        order.push_back({row.expire_at, row.mail_id});
    }
    std::sort(order.begin(), order.end());

    int ok_n = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        const uint64_t mid = order[i].second;
        std::string key = req.idempotency_key_prefix();
        if (key.empty())
            key = "batch_claim";
        key += ":" + std::to_string(req.player_id()) + ":" + std::to_string(mid);
        game::MailClaimResult r;
        if (ClaimOne(req.player_id(), mid, key, req.trace_id(), &r))
            ++ok_n;
        *body->add_results() = r;
    }
    body->set_ok(ok_n > 0);
    body->set_error_code(mail::err::kOk);
    body->set_message("batch done");
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    rsp->set_ok(true);
    rsp->set_message("batch done");
    return true;
}

bool MailService::HandleMailFavorite(const game::MailFavoriteReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_favorite();
    body->set_server_time_utc(NowUtc());
    mail::MailInstanceRow row;
    if (!MailStore::Instance().LoadMail(req.mail_id(), &row) ||
        row.receiver_id != req.player_id()) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kPermissionDenied);
        body->set_message("denied");
        rsp->set_message(body->message());
        return false;
    }
    if (req.favorite()) {
        int fav = 0;
        MailStore::Instance().CountFavorites(req.player_id(), &fav);
        if (!row.is_favorite && fav >= MailConfig::Instance().Values().favorite_max) {
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kFavoriteLimit);
            body->set_message("favorite limit");
            rsp->set_message(body->message());
            return false;
        }
    }
    auto conn = MailStore::Instance().GetConnection();
    if (!conn) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("db");
        rsp->set_message(body->message());
        return false;
    }
    row.is_favorite = req.favorite();
    row.updated_at = NowUtc();
    row.row_version += 1;
    if (!MailStore::Instance().UpdateMailRow(conn.get(), row)) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("update failed");
        rsp->set_message(body->message());
        return false;
    }
    if (!req.idempotency_key().empty()) {
        MailOpLogRow op;
        op.mail_id = row.mail_id;
        op.actor_id = req.player_id();
        op.operation_type = req.favorite() ? "FAVORITE" : "UNFAVORITE";
        op.idempotency_key = req.idempotency_key();
        op.before_state = "{}";
        op.after_state = req.favorite() ? "{\"favorite\":1}" : "{\"favorite\":0}";
        op.result_code = mail::err::kOk;
        op.created_at = NowUtc();
        MailStore::Instance().InsertOpLog(conn.get(), op);
    }
    BumpVersion(req.player_id());
    body->set_ok(true);
    body->set_error_code(mail::err::kOk);
    body->set_message("ok");
    body->set_is_favorite(row.is_favorite);
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool MailService::HandleMailBatchRead(const game::MailBatchReadReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_batch_read();
    body->set_server_time_utc(NowUtc());
    const int maxn = MailConfig::Instance().Values().batch_op_max;
    if (req.mail_ids_size() <= 0 || req.mail_ids_size() > maxn) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInvalidArgument);
        body->set_message("batch size");
        rsp->set_message(body->message());
        return false;
    }
    uint32_t ok_n = 0;
    for (int i = 0; i < req.mail_ids_size(); ++i) {
        game::GameResponse tmp;
        game::MailReadReq one;
        one.set_player_id(req.player_id());
        one.set_mail_id(req.mail_ids(i));
        one.set_idempotency_key(req.idempotency_key() + ":read:" + std::to_string(req.mail_ids(i)));
        if (HandleMailRead(one, &tmp))
            ++ok_n;
    }
    body->set_ok(true);
    body->set_error_code(mail::err::kOk);
    body->set_message("ok");
    body->set_success_count(ok_n);
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool MailService::HandleMailBatchDelete(const game::MailBatchDeleteReq &req,
                                        game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_batch_delete();
    body->set_server_time_utc(NowUtc());
    const int maxn = MailConfig::Instance().Values().batch_op_max;
    if (req.mail_ids_size() <= 0 || req.mail_ids_size() > maxn) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInvalidArgument);
        body->set_message("batch size");
        rsp->set_message(body->message());
        return false;
    }
    uint32_t ok_n = 0;
    const int64_t now = NowUtc();
    for (int i = 0; i < req.mail_ids_size(); ++i) {
        const uint64_t mid = req.mail_ids(i);
        mail::MailInstanceRow row;
        if (!MailStore::Instance().LoadMail(mid, &row) || row.receiver_id != req.player_id()) {
            body->add_failed_mail_ids(mid);
            continue;
        }
        if (row.attachment_state == "UNCLAIMED" || row.attachment_state == "CLAIMING") {
            body->add_failed_mail_ids(mid);
            continue;
        }
        if (SoftDeleteMail(mid, req.player_id(), now))
            ++ok_n;
        else
            body->add_failed_mail_ids(mid);
    }
    BumpVersion(req.player_id());
    body->set_ok(true);
    body->set_error_code(mail::err::kOk);
    body->set_message("ok");
    body->set_success_count(ok_n);
    body->set_mailbox_version(MailboxVersion(req.player_id()));
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool MailService::HandleMailDeliver(const game::MailDeliverReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_mail_deliver();
    body->set_server_time_utc(NowUtc());
    mail::DeliverRequest d;
    d.source_system = req.source_system();
    d.business_key = req.business_key();
    d.receiver_type = req.receiver_type().empty() ? "ROLE" : req.receiver_type();
    d.receiver_id = req.receiver_id();
    d.template_id = req.template_id();
    d.template_version = req.template_version();
    d.category = req.category().empty() ? "SYSTEM" : req.category();
    d.priority = req.priority();
    d.sender_name = req.sender_name();
    d.title = req.title();
    d.body = req.body();
    d.send_at = req.send_at();
    d.expire_at = req.expire_at();
    d.trace_id = req.trace_id();
    for (int i = 0; i < req.attachments_size(); ++i) {
        mail::DeliverAttachment a;
        a.asset_type = req.attachments(i).asset_type().empty() ? "ITEM"
                                                               : req.attachments(i).asset_type();
        a.asset_id = req.attachments(i).asset_id();
        a.count = req.attachments(i).count();
        a.bind_type = req.attachments(i).bind_type();
        a.payload = req.attachments(i).payload();
        d.attachments.push_back(a);
    }
    uint64_t mail_id = 0;
    std::string ec, msg;
    const bool ok = Deliver(d, &mail_id, &ec, &msg);
    body->set_ok(ok);
    body->set_error_code(ec);
    body->set_message(msg);
    body->set_mail_id(mail_id);
    body->set_idempotent_hit(ok && msg.find("idempotent") != std::string::npos);
    rsp->set_ok(ok);
    rsp->set_message(msg);
    return ok;
}

int MailService::ScanExpire(int limit) {
    if (!ready_ && !Init())
        return 0;
    int n = 0;
    MailStore::Instance().MarkExpiredBatch(NowUtc(), limit, &n);
    return n;
}
