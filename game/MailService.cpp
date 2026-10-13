#include "MailService.h"

#include "AsyncMysqlGameDbRepository.h"
#include "Connection.h"
#include "GameDbRepository.h"
#include "FormalMode.h"
#include "GameLogic.h"
#include "IGameDbRepository.h"
#include "Logging.h"
#include "MailConfig.h"
#include "MailStore.h"
#include "PlayerAccountStore.h"
#include "PlayerProfileStore.h"
#include "PlayerSerialQueue.h"
#include "Utf8Text.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "RedisPool.h"
#include "SessionStore.h"
#endif
#ifdef WEBSERVER_ENABLE_BRPC
#include "GameLogicPush.h"
#include "SessionRpcClient.h"
#include "session.pb.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace {

constexpr int64_t kInventorySoftCap = 999999;

bool DeliverPayloadMatches(const mail::MailInstanceRow &existing, const mail::DeliverRequest &req,
                           mail::Category cat, std::string *error) {
    const std::string expect_name = req.sender_name.empty() ? "System" : req.sender_name;
    const std::string expect_sender_type = req.sender_type.empty() ? "SYSTEM" : req.sender_type;
    const int expect_template_version = req.template_version > 0 ? req.template_version : 1;
    if (existing.receiver_id != req.receiver_id || existing.title_snapshot != req.title ||
        existing.body_snapshot != req.body || existing.category != mail::ToString(cat) ||
        existing.sender_id != req.sender_id || existing.sender_type != expect_sender_type ||
        existing.sender_name_snapshot != expect_name || existing.template_id != req.template_id ||
        existing.template_version != expect_template_version || existing.priority != req.priority) {
        if (error)
            *error = "same business_key with different content";
        return false;
    }
    if (req.expire_at > 0 && existing.expire_at != req.expire_at) {
        if (error)
            *error = "same business_key with different expire_at";
        return false;
    }
    if (req.send_at > 0 && existing.sent_at != req.send_at) {
        if (error)
            *error = "same business_key with different send_at";
        return false;
    }
    std::vector<mail::MailAttachmentRow> stored;
    if (!MailStore::Instance().LoadAttachments(existing.mail_id, &stored)) {
        if (error)
            *error = "load attachments failed";
        return false;
    }
    if (stored.size() != req.attachments.size()) {
        if (error)
            *error = "same business_key with different attachments";
        return false;
    }
    for (size_t i = 0; i < stored.size(); ++i) {
        const auto &got = stored[i];
        const auto &want = req.attachments[i];
        if (mail::AttachmentFingerprint(got.asset_type, got.asset_id, got.count, got.bind_type,
                                        got.payload) !=
            mail::AttachmentFingerprint(want.asset_type, want.asset_id, want.count, want.bind_type,
                                        want.payload)) {
            if (error)
                *error = "same business_key with different attachments";
            return false;
        }
    }
    return true;
}

bool ReplayOpOrConflict(const MailOpLogRow &existed, uint64_t actor, uint64_t mail_id,
                        const char *op, std::string *error_code, std::string *message) {
    if (!mail::OpIntentMatches(existed.actor_id, existed.mail_id, existed.operation_type, actor,
                               mail_id, op)) {
        if (error_code)
            *error_code = mail::err::kIdempotencyConflict;
        if (message)
            *message = "idempotency key reused";
        return false;
    }
    if (error_code)
        *error_code = existed.result_code;
    if (message)
        *message = "idempotent";
    return existed.result_code == mail::err::kOk;
}

bool PayloadNeedsManualClaim(const std::vector<mail::MailAttachmentRow> &atts) {
    for (const auto &a : atts) {
        if (a.payload.find("need_confirm") != std::string::npos ||
            a.payload.find("COD") != std::string::npos)
            return true;
    }
    return false;
}

void FillManualClaimSkip(uint64_t mail_id, game::MailClaimResult *result) {
    result->set_mail_id(mail_id);
    result->set_ok(false);
    result->set_error_code(mail::err::kInvalidState);
    result->set_message("manual confirm required");
}

void FillClaimReceipt(const GameDbMailClaimResult &db, game::MailClaimResult *result) {
    auto *receipt = result->mutable_receipt();
    std::string state = db.attachment_state;
    if (state == "CLAIMING")
        state = "IN_PROGRESS";
    else if (state.empty())
        state = "UNKNOWN";
    receipt->set_claim_state(state);
    receipt->set_claim_tx_id(db.claim_tx_id);
    receipt->set_asset_version_at_claim(db.asset_version_at_claim);
    receipt->set_verifiable(db.receipt_verifiable);
    if (!db.receipt_verifiable)
        return;
    for (const auto &g : db.grants) {
        auto *item = receipt->add_grants();
        item->set_asset_id(g.asset_id);
        item->set_count(g.count);
    }
}

bool FillDetailReceipt(uint64_t player_id, const mail::MailInstanceRow &row, game::MailDetail *detail) {
    auto *receipt = detail->mutable_receipt();
    if (row.attachment_state == "CLAIMED") {
        MailClaimReceiptData data;
        const MailLookup src =
            MailStore::Instance().FindOriginalClaimReceipt(row.mail_id, player_id, &data);
        if (src == MailLookup::kError)
            return false;
        receipt->set_claim_state("CLAIMED");
        receipt->set_verifiable(src == MailLookup::kHit && data.verifiable);
        if (!receipt->verifiable())
            return true;
        receipt->set_claim_tx_id(data.claim_tx_id);
        receipt->set_asset_version_at_claim(data.asset_version_at_claim);
        for (const auto &g : data.grants) {
            auto *item = receipt->add_grants();
            item->set_asset_id(g.asset_id);
            item->set_count(g.count);
        }
        return true;
    }
    if (row.attachment_state == "CLAIMING")
        receipt->set_claim_state("IN_PROGRESS");
    else if (row.attachment_state == "UNCLAIMED")
        receipt->set_claim_state("UNCLAIMED");
    else
        receipt->set_claim_state(row.attachment_state.empty() ? "UNKNOWN" : row.attachment_state);
    receipt->set_verifiable(false);
    return true;
}

std::string CanonicalMailIds(const google::protobuf::RepeatedField<uint64_t> &ids) {
    std::set<uint64_t> uniq(ids.begin(), ids.end());
    std::string out;
    for (uint64_t id : uniq) {
        if (!out.empty())
            out.push_back(',');
        out += std::to_string(id);
    }
    return out;
}

void FillBatchDeleteReplay(const std::string &after, game::MailBatchDeleteRsp *body) {
    const std::string mark = ";failed=";
    const auto pos = after.find(mark);
    if (pos == std::string::npos)
        return;
    const std::string failed = after.substr(pos + mark.size());
    std::stringstream ss(failed);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty())
            body->add_failed_mail_ids(std::strtoull(item.c_str(), nullptr, 10));
    }
    const auto okpos = after.find(";ok=");
    if (okpos != std::string::npos) {
        body->set_success_count(static_cast<uint32_t>(std::strtoul(
            after.c_str() + okpos + 4, nullptr, 10)));
    }
}

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
    // 独立 GameDB 时由 Bootstrap 先 Set(BrpcGameDbRepository)；否则起本地 AsyncMysql
    if (!GameDbRepository::Get()) {
        if (!AsyncMysqlGameDbRepository::Instance().started())
            AsyncMysqlGameDbRepository::Instance().Start(2);
    }
    if (!GameDbRepository::Get()) {
        LOG_WARN << "MailService: GameDB not available";
        ready_ = false;
        return false;
    }
    ready_ = true;
    LOG_INFO << "MailService: ready (GameDB claim path)";
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
    if (mail::PreserveAttachmentOnExpire(row->attachment_state))
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
    const MailLookup prior = MailStore::Instance().FindByBusinessKey(
        req.source_system, req.business_key, req.receiver_id, &existing);
    if (prior == MailLookup::kError) {
        *error_code = mail::err::kInternal;
        *message = "idempotency lookup failed";
        return false;
    }
    if (prior == MailLookup::kHit) {
        std::string mismatch;
        if (!DeliverPayloadMatches(existing, req, cat, &mismatch)) {
            *error_code = mismatch == "load attachments failed" ? mail::err::kInternal
                                                                : mail::err::kIdempotencyConflict;
            *message = mismatch;
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
    row.sender_type = req.sender_type.empty() ? "SYSTEM" : req.sender_type;
    row.sender_id = req.sender_id;
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
            const MailLookup again = MailStore::Instance().FindByBusinessKey(
                req.source_system, req.business_key, req.receiver_id, &existing);
            if (again == MailLookup::kError) {
                *error_code = mail::err::kInternal;
                *message = "idempotency lookup failed";
                return false;
            }
            if (again == MailLookup::kHit) {
                std::string mismatch;
                if (!DeliverPayloadMatches(existing, req, cat, &mismatch)) {
                    *error_code = mismatch == "load attachments failed"
                                      ? mail::err::kInternal
                                      : mail::err::kIdempotencyConflict;
                    *message = mismatch;
                    return false;
                }
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
    const MailLookup loaded = MailStore::Instance().LookupMail(req.mail_id(), &row);
    if (loaded == MailLookup::kError) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("mail lookup failed");
        rsp->set_message(body->message());
        return false;
    }
    if (loaded == MailLookup::kMiss) {
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
    if (!MailStore::Instance().LoadAttachments(row.mail_id, &atts)) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("attachment lookup failed");
        rsp->set_message(body->message());
        return false;
    }
    FillMailDetail(row, atts, body->mutable_mail());
    if (!FillDetailReceipt(req.player_id(), row, body->mutable_mail())) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("claim receipt lookup failed");
        rsp->set_message(body->message());
        return false;
    }
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
    const MailLookup prior =
        MailStore::Instance().FindOpByIdempotency(req.idempotency_key(), &existed);
    if (prior == MailLookup::kError) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("idempotency lookup failed");
        rsp->set_message(body->message());
        return false;
    }
    if (prior == MailLookup::kHit) {
        std::string ec, msg;
        const bool replay = ReplayOpOrConflict(existed, req.player_id(), req.mail_id(), "READ", &ec,
                                                &msg);
        rsp->set_ok(replay);
        body->set_ok(replay);
        body->set_error_code(ec);
        body->set_message(msg);
        body->set_mailbox_version(MailboxVersion(req.player_id()));
        rsp->set_message(body->message());
        return replay;
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
    if (!MailStore::Instance().Begin(conn.get())) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("begin failed");
        rsp->set_message(body->message());
        return false;
    }
    if (row.read_state != "READ") {
        row.read_state = "READ";
        row.read_at = now;
        row.updated_at = now;
        row.row_version += 1;
        if (!MailStore::Instance().UpdateMailRow(conn.get(), row)) {
            MailStore::Instance().Rollback(conn.get());
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
    if (!MailStore::Instance().InsertOpLog(conn.get(), op) ||
        !MailStore::Instance().Commit(conn.get())) {
        MailStore::Instance().Rollback(conn.get());
        MailOpLogRow raced;
        const MailLookup again =
            MailStore::Instance().FindOpByIdempotency(req.idempotency_key(), &raced);
        if (again == MailLookup::kHit) {
            std::string ec, msg;
            const bool replay =
                ReplayOpOrConflict(raced, req.player_id(), req.mail_id(), "READ", &ec, &msg);
            rsp->set_ok(replay);
            body->set_ok(replay);
            body->set_error_code(ec);
            body->set_message(msg);
            rsp->set_message(msg);
            return replay;
        }
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("idempotency write failed");
        rsp->set_message(body->message());
        return false;
    }
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
    auto *repo = GameDbRepository::Get();
    if (!repo) {
        result->set_error_code(mail::err::kInternal);
        result->set_message("gamedb unavailable");
        return false;
    }
    if (!GameLogic::Instance().EnsureAssetsSynced(player_id)) {
        result->set_error_code(mail::err::kStateSyncRequired);
        result->set_message("asset memory sync required");
        return false;
    }

    GameDbMailClaimRequest db_req;
    db_req.player_id = player_id;
    db_req.mail_id = mail_id;
    db_req.idempotency_key = idempotency_key;
    db_req.trace_id = trace_id;
    db_req.inventory_soft_cap = kInventorySoftCap;
    GameLogic::Instance().CopyInventory(player_id, &db_req.bag_snapshot);

    // 非玩家串行路径（MailBrpc/单测）：同步接口，禁止 promise+future.get
    GameDbMailClaimResult db_rsp = repo->ClaimMailAttachments(std::move(db_req));

    result->set_ok(db_rsp.ok);
    result->set_error_code(db_rsp.error_code);
    result->set_message(db_rsp.message);
    result->set_attachment_state(db_rsp.attachment_state);
    FillClaimReceipt(db_rsp, result);
    if (!ApplyClaimMemory(player_id, db_rsp)) {
        result->set_ok(false);
        result->set_error_code(mail::err::kStateSyncRequired);
        result->set_message("asset memory sync required");
        return false;
    }
    return db_rsp.ok;
}

bool MailService::ApplyClaimMemory(uint64_t player_id, const GameDbMailClaimResult &db_rsp) {
    if (!db_rsp.ok)
        return true;
    bool aligned = false;
    if (db_rsp.should_apply_memory) {
        if (db_rsp.asset_version != 0) {
            aligned = GameLogic::Instance().ApplyItemRewardsWithVersion(player_id, db_rsp.grants,
                                                                        db_rsp.asset_version);
        } else {
            aligned = true;
            for (const auto &g : db_rsp.grants) {
                if (!GameLogic::Instance().ApplyItemReward(player_id,
                                                           static_cast<uint32_t>(g.asset_id),
                                                           g.count))
                    aligned = false;
            }
        }
    } else {
        aligned = GameLogic::Instance().ReloadAuthoritativeAssets(player_id);
    }
    if (!aligned)
        return false;
    BumpVersion(player_id);
    return true;
}

bool MailService::BeginHandleMailClaimAsync(const game::MailClaimReq &req, game::GameResponse *rsp,
                                            std::function<void(game::GameResponse)> done) {
    if (!rsp || !done)
        return false;
    auto *body = rsp->mutable_mail_claim();
    body->set_server_time_utc(NowUtc());
    auto fail_sync = [&](const std::string &code, const std::string &msg) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(code);
        body->set_message(msg);
        rsp->set_message(msg);
    };
    if (!ready_ && !Init()) {
        fail_sync(mail::err::kInternal, "mail not ready");
        return false;
    }
    if (req.idempotency_key().empty()) {
        fail_sync(mail::err::kInvalidArgument, "idempotency_key required");
        return false;
    }
    auto *repo = GameDbRepository::Get();
    if (!repo) {
        fail_sync(mail::err::kInternal, "gamedb unavailable");
        return false;
    }

    GameDbMailClaimRequest db_req;
    db_req.player_id = req.player_id();
    db_req.mail_id = req.mail_id();
    db_req.idempotency_key = req.idempotency_key();
    db_req.trace_id = req.trace_id();
    db_req.inventory_soft_cap = kInventorySoftCap;
    GameLogic::Instance().CopyInventory(req.player_id(), &db_req.bag_snapshot);

    const uint64_t player_id = req.player_id();
    const uint64_t mail_id = req.mail_id();
    repo->ClaimMailAttachmentsAsync(
        std::move(db_req), [player_id, mail_id, done = std::move(done)](GameDbMailClaimResult db_rsp) {
            auto apply = std::make_shared<std::function<void()>>();
            *apply = [player_id, mail_id, db_rsp = std::move(db_rsp), done = std::move(done)]() mutable {
                const bool mem_ok = MailService::Instance().ApplyClaimMemory(player_id, db_rsp);
                game::GameResponse out;
                auto *body = out.mutable_mail_claim();
                using namespace std::chrono;
                body->set_server_time_utc(
                    duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
                game::MailClaimResult result;
                result.set_mail_id(mail_id);
                const bool ok = db_rsp.ok && mem_ok;
                if (db_rsp.ok && !mem_ok) {
                    result.set_ok(false);
                    result.set_error_code(mail::err::kStateSyncRequired);
                    result.set_message("asset memory sync required");
                } else {
                    result.set_ok(db_rsp.ok);
                    result.set_error_code(db_rsp.error_code);
                    result.set_message(db_rsp.message);
                }
                result.set_attachment_state(db_rsp.attachment_state);
                FillClaimReceipt(db_rsp, &result);
                *body->mutable_result() = result;
                body->set_ok(ok);
                body->set_error_code(result.error_code());
                body->set_message(result.message());
                body->set_mailbox_version(MailService::Instance().MailboxVersion(player_id));
                out.set_ok(ok);
                out.set_message(result.message());
                done(std::move(out));
            };
            if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(player_id,
                                                                     [apply]() { (*apply)(); })) {
                // STOPPED：不改玩家状态；brpc done 恰好一次
                game::GameResponse out;
                auto *body = out.mutable_mail_claim();
                using namespace std::chrono;
                body->set_server_time_utc(
                    duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
                body->set_ok(false);
                body->set_error_code(mail::err::kServerStopping);
                body->set_message("server stopping");
                out.set_ok(false);
                out.set_message("server stopping");
                done(std::move(out));
            }
        });
    return true;
}

bool MailService::BeginHandleMailBatchClaimAsync(const game::MailBatchClaimReq &req,
                                                 game::GameResponse *rsp,
                                                 std::function<void(game::GameResponse)> done) {
    if (!rsp || !done)
        return false;
    auto *body = rsp->mutable_mail_batch_claim();
    body->set_server_time_utc(NowUtc());
    auto fail_sync = [&](const std::string &code, const std::string &msg) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(code);
        body->set_message(msg);
        rsp->set_message(msg);
    };
    if (!ready_ && !Init()) {
        fail_sync(mail::err::kInternal, "mail not ready");
        return false;
    }
    const auto &cfg = MailConfig::Instance().Values();
    if (req.mail_ids_size() == 0 || req.mail_ids_size() > cfg.batch_op_max) {
        fail_sync(mail::err::kInvalidArgument, "mail_ids size invalid");
        return false;
    }
    auto *repo = GameDbRepository::Get();
    if (!repo) {
        fail_sync(mail::err::kInternal, "gamedb unavailable");
        return false;
    }

    struct BatchState {
        uint64_t player_id = 0;
        std::string trace_id;
        std::string key_prefix;
        std::vector<uint64_t> mail_ids;
        size_t index = 0;
        std::vector<game::MailClaimResult> results;
        std::function<void(game::GameResponse)> done;
    };
    auto st = std::make_shared<BatchState>();
    st->player_id = req.player_id();
    st->trace_id = req.trace_id();
    st->key_prefix = req.idempotency_key_prefix().empty() ? "batch_claim" : req.idempotency_key_prefix();
    st->done = std::move(done);

    std::vector<std::pair<int64_t, uint64_t>> order;
    for (int i = 0; i < req.mail_ids_size(); ++i) {
        mail::MailInstanceRow row;
        const MailLookup loaded = MailStore::Instance().LookupMail(req.mail_ids(i), &row);
        if (loaded != MailLookup::kHit || row.receiver_id != req.player_id()) {
            game::MailClaimResult denied;
            denied.set_mail_id(req.mail_ids(i));
            denied.set_ok(false);
            if (loaded == MailLookup::kError) {
                denied.set_error_code(mail::err::kInternal);
                denied.set_message("mail lookup failed");
            } else if (loaded == MailLookup::kMiss) {
                denied.set_error_code(mail::err::kMailNotFound);
                denied.set_message("not found");
            } else {
                denied.set_error_code(mail::err::kPermissionDenied);
                denied.set_message("permission denied");
            }
            st->results.push_back(denied);
            continue;
        }
        std::vector<mail::MailAttachmentRow> atts;
        if (!MailStore::Instance().LoadAttachments(row.mail_id, &atts)) {
            game::MailClaimResult denied;
            denied.set_mail_id(row.mail_id);
            denied.set_ok(false);
            denied.set_error_code(mail::err::kInternal);
            denied.set_message("attachment lookup failed");
            st->results.push_back(denied);
            continue;
        }
        if (PayloadNeedsManualClaim(atts)) {
            game::MailClaimResult skipped;
            FillManualClaimSkip(row.mail_id, &skipped);
            st->results.push_back(skipped);
            continue;
        }
        order.push_back({row.expire_at, row.mail_id});
    }
    std::sort(order.begin(), order.end());
    for (const auto &p : order)
        st->mail_ids.push_back(p.second);

    auto finish = [](const std::shared_ptr<BatchState> &s) {
        game::GameResponse out;
        auto *b = out.mutable_mail_batch_claim();
        using namespace std::chrono;
        b->set_server_time_utc(
            duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
        int ok_n = 0;
        for (const auto &r : s->results) {
            if (r.ok())
                ++ok_n;
            *b->add_results() = r;
        }
        b->set_ok(ok_n > 0);
        b->set_error_code(mail::err::kOk);
        b->set_message("batch done");
        b->set_mailbox_version(MailService::Instance().MailboxVersion(s->player_id));
        out.set_ok(true);
        out.set_message("batch done");
        s->done(std::move(out));
    };

    auto start_next = std::make_shared<std::function<void()>>();
    *start_next = [st, start_next, finish, repo]() {
        if (st->index >= st->mail_ids.size()) {
            if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(st->player_id, [st, finish]() {
                    finish(st);
                })) {
                game::GameResponse out;
                auto *b = out.mutable_mail_batch_claim();
                using namespace std::chrono;
                b->set_server_time_utc(
                    duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
                b->set_ok(false);
                b->set_error_code(mail::err::kServerStopping);
                b->set_message("server stopping");
                out.set_ok(false);
                out.set_message("server stopping");
                st->done(std::move(out));
            }
            return;
        }
        const uint64_t mid = st->mail_ids[st->index];
        std::string key = st->key_prefix + ":" + std::to_string(st->player_id) + ":" +
                          std::to_string(mid);

        GameDbMailClaimRequest db_req;
        db_req.player_id = st->player_id;
        db_req.mail_id = mid;
        db_req.idempotency_key = key;
        db_req.trace_id = st->trace_id;
        db_req.inventory_soft_cap = kInventorySoftCap;
        GameLogic::Instance().CopyInventory(st->player_id, &db_req.bag_snapshot);

        repo->ClaimMailAttachmentsAsync(
            std::move(db_req), [st, start_next, mid](GameDbMailClaimResult db_rsp) {
                if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                        st->player_id, [st, start_next, mid, db_rsp = std::move(db_rsp)]() mutable {
                            const bool mem_ok =
                                MailService::Instance().ApplyClaimMemory(st->player_id, db_rsp);
                            game::MailClaimResult r;
                            r.set_mail_id(mid);
                            if (db_rsp.ok && !mem_ok) {
                                r.set_ok(false);
                                r.set_error_code(mail::err::kStateSyncRequired);
                                r.set_message("asset memory sync required");
                            } else {
                                r.set_ok(db_rsp.ok);
                                r.set_error_code(db_rsp.error_code);
                                r.set_message(db_rsp.message);
                            }
                            r.set_attachment_state(db_rsp.attachment_state);
                            FillClaimReceipt(db_rsp, &r);
                            st->results.push_back(std::move(r));
                            ++st->index;
                            // Complete 已清 inflight；下一封 RPC 前重新标记
                            PlayerSerialQueue::Instance().MarkAsyncInFlight(st->player_id);
                            (*start_next)();
                        })) {
                    game::GameResponse out;
                    auto *b = out.mutable_mail_batch_claim();
                    using namespace std::chrono;
                    b->set_server_time_utc(
                        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
                    b->set_ok(false);
                    b->set_error_code(mail::err::kServerStopping);
                    b->set_message("server stopping");
                    out.set_ok(false);
                    out.set_message("server stopping");
                    st->done(std::move(out));
                }
            });
    };

    (*start_next)();
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
        const MailLookup loaded = MailStore::Instance().LookupMail(req.mail_ids(i), &row);
        if (loaded != MailLookup::kHit || row.receiver_id != req.player_id()) {
            game::MailClaimResult denied;
            denied.set_mail_id(req.mail_ids(i));
            denied.set_ok(false);
            if (loaded == MailLookup::kError) {
                denied.set_error_code(mail::err::kInternal);
                denied.set_message("mail lookup failed");
            } else if (loaded == MailLookup::kMiss) {
                denied.set_error_code(mail::err::kMailNotFound);
                denied.set_message("not found");
            } else {
                denied.set_error_code(mail::err::kPermissionDenied);
                denied.set_message("permission denied");
            }
            *body->add_results() = denied;
            continue;
        }
        std::vector<mail::MailAttachmentRow> atts;
        if (!MailStore::Instance().LoadAttachments(row.mail_id, &atts)) {
            game::MailClaimResult denied;
            denied.set_mail_id(row.mail_id);
            denied.set_ok(false);
            denied.set_error_code(mail::err::kInternal);
            denied.set_message("attachment lookup failed");
            *body->add_results() = denied;
            continue;
        }
        if (PayloadNeedsManualClaim(atts)) {
            game::MailClaimResult skipped;
            FillManualClaimSkip(row.mail_id, &skipped);
            *body->add_results() = skipped;
            continue;
        }
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
    const char *fav_op = req.favorite() ? "FAVORITE" : "UNFAVORITE";
    if (!req.idempotency_key().empty()) {
        MailOpLogRow existed;
        const MailLookup prior =
            MailStore::Instance().FindOpByIdempotency(req.idempotency_key(), &existed);
        if (prior == MailLookup::kError) {
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("idempotency lookup failed");
            rsp->set_message(body->message());
            return false;
        }
        if (prior == MailLookup::kHit) {
            std::string ec, msg;
            const bool replay =
                ReplayOpOrConflict(existed, req.player_id(), req.mail_id(), fav_op, &ec, &msg);
            rsp->set_ok(replay);
            body->set_ok(replay);
            body->set_error_code(ec);
            body->set_message(msg);
            body->set_is_favorite(req.favorite());
            body->set_mailbox_version(MailboxVersion(req.player_id()));
            rsp->set_message(msg);
            return replay;
        }
    }
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
    auto conn = MailStore::Instance().GetConnection();
    if (!conn || !MailStore::Instance().Begin(conn.get())) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("db");
        rsp->set_message(body->message());
        return false;
    }
    mail::MailInstanceRow locked;
    const MailLookup locked_lookup =
        MailStore::Instance().LookupMailForUpdate(conn.get(), req.mail_id(), &locked);
    if (locked_lookup != MailLookup::kHit || locked.receiver_id != req.player_id()) {
        MailStore::Instance().Rollback(conn.get());
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(locked_lookup == MailLookup::kError ? mail::err::kInternal
                                                                 : mail::err::kPermissionDenied);
        body->set_message("denied");
        rsp->set_message(body->message());
        return false;
    }
    if (req.favorite() && !locked.is_favorite) {
        int fav = 0;
        if (!MailStore::Instance().CountFavoritesForUpdate(conn.get(), req.player_id(), &fav)) {
            MailStore::Instance().Rollback(conn.get());
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("favorite count failed");
            rsp->set_message(body->message());
            return false;
        }
        if (fav >= MailConfig::Instance().Values().favorite_max) {
            MailStore::Instance().Rollback(conn.get());
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kFavoriteLimit);
            body->set_message("favorite limit");
            rsp->set_message(body->message());
            return false;
        }
    }
    locked.is_favorite = req.favorite();
    locked.updated_at = NowUtc();
    locked.row_version += 1;
    if (!MailStore::Instance().UpdateMailRow(conn.get(), locked)) {
        MailStore::Instance().Rollback(conn.get());
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("update failed");
        rsp->set_message(body->message());
        return false;
    }
    if (!req.idempotency_key().empty()) {
        MailOpLogRow op;
        op.mail_id = locked.mail_id;
        op.actor_id = req.player_id();
        op.operation_type = fav_op;
        op.idempotency_key = req.idempotency_key();
        op.before_state = "{}";
        op.after_state = req.favorite() ? "{\"favorite\":1}" : "{\"favorite\":0}";
        op.result_code = mail::err::kOk;
        op.created_at = NowUtc();
        if (!MailStore::Instance().InsertOpLog(conn.get(), op)) {
            MailStore::Instance().Rollback(conn.get());
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("idempotency write failed");
            rsp->set_message(body->message());
            return false;
        }
    }
    if (!MailStore::Instance().Commit(conn.get())) {
        MailStore::Instance().Rollback(conn.get());
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("commit failed");
        rsp->set_message(body->message());
        return false;
    }
    row = locked;
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
        one.set_idempotency_key(req.idempotency_key() + ":" + std::to_string(req.player_id()) +
                                ":read:" + std::to_string(req.mail_ids(i)));
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
    const std::string fingerprint = CanonicalMailIds(req.mail_ids());
    const uint64_t anchor = fingerprint.empty()
                                ? 0
                                : static_cast<uint64_t>(std::strtoull(fingerprint.c_str(), nullptr, 10));
    if (!req.idempotency_key().empty()) {
        MailOpLogRow existed;
        const MailLookup prior =
            MailStore::Instance().FindOpByIdempotency(req.idempotency_key(), &existed);
        if (prior == MailLookup::kError) {
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("idempotency lookup failed");
            rsp->set_message(body->message());
            return false;
        }
        if (prior == MailLookup::kHit) {
            if (!mail::OpIntentMatches(existed.actor_id, existed.mail_id, existed.operation_type,
                                       req.player_id(), anchor, "BATCH_DELETE") ||
                existed.after_state.find("ids=" + fingerprint + ";") != 0) {
                rsp->set_ok(false);
                body->set_ok(false);
                body->set_error_code(mail::err::kIdempotencyConflict);
                body->set_message("idempotency key reused");
                rsp->set_message(body->message());
                return false;
            }
            FillBatchDeleteReplay(existed.after_state, body);
            body->set_ok(existed.result_code == mail::err::kOk);
            body->set_error_code(existed.result_code);
            body->set_message("idempotent");
            body->set_mailbox_version(MailboxVersion(req.player_id()));
            rsp->set_ok(body->ok());
            rsp->set_message(body->message());
            return body->ok();
        }
    }
    const int64_t now = NowUtc();
    auto conn = MailStore::Instance().GetConnection();
    if (!conn || !MailStore::Instance().Begin(conn.get())) {
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("db");
        rsp->set_message(body->message());
        return false;
    }
    uint32_t ok_n = 0;
    std::vector<uint64_t> failed;
    std::set<uint64_t> seen;
    for (int i = 0; i < req.mail_ids_size(); ++i)
        seen.insert(req.mail_ids(i));
    for (uint64_t mid : seen) {
        mail::MailInstanceRow row;
        const MailLookup loaded = MailStore::Instance().LookupMailForUpdate(conn.get(), mid, &row);
        if (loaded == MailLookup::kError) {
            MailStore::Instance().Rollback(conn.get());
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("mail lookup failed");
            rsp->set_message(body->message());
            return false;
        }
        if (loaded != MailLookup::kHit || row.receiver_id != req.player_id() ||
            row.attachment_state == "UNCLAIMED" || row.attachment_state == "CLAIMING" ||
            row.visible_state != "ACTIVE") {
            failed.push_back(mid);
            continue;
        }
        row.visible_state = "SOFT_DELETED";
        row.deleted_at = now;
        row.updated_at = now;
        row.row_version += 1;
        if (!MailStore::Instance().UpdateMailRow(conn.get(), row)) {
            MailStore::Instance().Rollback(conn.get());
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("delete failed");
            rsp->set_message(body->message());
            return false;
        }
        ++ok_n;
    }
    std::string failed_text;
    for (uint64_t mid : failed) {
        if (!failed_text.empty())
            failed_text.push_back(',');
        failed_text += std::to_string(mid);
        body->add_failed_mail_ids(mid);
    }
    if (!req.idempotency_key().empty()) {
        MailOpLogRow op;
        op.mail_id = anchor;
        op.actor_id = req.player_id();
        op.operation_type = "BATCH_DELETE";
        op.idempotency_key = req.idempotency_key();
        op.before_state = "{}";
        op.after_state = "ids=" + fingerprint + ";ok=" + std::to_string(ok_n) + ";failed=" + failed_text;
        op.result_code = mail::err::kOk;
        op.created_at = now;
        if (!MailStore::Instance().InsertOpLog(conn.get(), op)) {
            MailStore::Instance().Rollback(conn.get());
            rsp->set_ok(false);
            body->set_ok(false);
            body->set_error_code(mail::err::kInternal);
            body->set_message("idempotency write failed");
            rsp->set_message(body->message());
            return false;
        }
    }
    if (!MailStore::Instance().Commit(conn.get())) {
        MailStore::Instance().Rollback(conn.get());
        rsp->set_ok(false);
        body->set_ok(false);
        body->set_error_code(mail::err::kInternal);
        body->set_message("commit failed");
        rsp->set_message(body->message());
        return false;
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

namespace {

bool ConsumePlayerMailQuota(uint64_t sender_id, int limit, int window_sec) {
    if (sender_id == 0 || limit <= 0)
        return true;
#ifdef WEBSERVER_ENABLE_REDIS
    if (RedisPool::Instance().ready()) {
        auto lease = RedisPool::Instance().Acquire();
        if (lease) {
            char key[192];
            std::snprintf(key, sizeof(key), "gamemesh:dev:mailrate:%llu",
                          static_cast<unsigned long long>(sender_id));
            static const char *kLua =
                "local c=redis.call('INCR', KEYS[1]); if c==1 then redis.call('EXPIRE', KEYS[1], "
                "tonumber(ARGV[1])) end; return c";
            std::vector<std::string> out;
            if (lease->Eval(kLua, {key}, {std::to_string(window_sec)}, &out) && !out.empty()) {
                const int n = std::atoi(out[0].c_str());
                return n <= limit;
            }
        }
    }
#endif
    return true;
}

void BestEffortMailboxChanged(uint64_t receiver_id) {
#ifdef WEBSERVER_ENABLE_BRPC
    if (receiver_id == 0)
        return;
    sess::GetPlayerRouteResponse rrsp;
    bool got = false;
    if (SessionRpcClient::Instance().ready()) {
        sess::GetPlayerRouteRequest rreq;
        rreq.set_player_id(receiver_id);
        got = SessionRpcClient::Instance().GetPlayerRoute(rreq, &rrsp) && rrsp.ok();
    }
#ifdef WEBSERVER_ENABLE_REDIS
    if (!got && SessionStore::Instance().Available()) {
        SessionRecord rec;
        std::string route_state, transfer_id, err;
        if (SessionStore::Instance().GetPlayerRoute(receiver_id, "", &rec, &route_state,
                                                    &transfer_id, &err)) {
            rrsp.set_ok(true);
            rrsp.set_session_id(rec.session_id);
            rrsp.set_fence_token(rec.token);
            rrsp.set_generation(rec.generation);
            rrsp.set_gateway_instance_id(rec.gateway_id);
            rrsp.set_route_state(route_state.empty() ? "ONLINE" : route_state);
            got = true;
        }
    }
#endif
    if (!got || !rrsp.ok())
        return;
    if (rrsp.gateway_instance_id().empty() || rrsp.session_id().empty())
        return;
    if (rrsp.route_state() == "DISCONNECTED" || rrsp.route_state() == "OFFLINE")
        return;
    int us = 0, ua = 0, uso = 0, ut = 0, unc = 0, exp = 0, cur = 0;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    (void)MailStore::Instance().Summarize(receiver_id, now, MailService::Instance().MailboxVersion(receiver_id),
                                          &us, &ua, &uso, &ut, &unc, &exp, &cur);
    const uint32_t unread = static_cast<uint32_t>(us + ua + uso + ut);
    game::GameResponse inner;
    inner.set_seq(0);
    inner.set_ok(true);
    auto *n = inner.mutable_mailbox_changed();
    n->set_player_id(receiver_id);
    n->set_mailbox_version(MailService::Instance().MailboxVersion(receiver_id));
    n->set_unread_count(unread);
    std::string payload;
    if (!inner.SerializeToString(&payload))
        return;
    GameLogicPush::PushToBoundGateway(rrsp.gateway_instance_id(), receiver_id, rrsp.session_id(),
                                      "mailbox.changed.v1", payload, true, false, 0,
                                      rrsp.fence_token(), rrsp.generation());
#else
    (void)receiver_id;
#endif
}

}  // namespace

bool MailService::HandlePlayerMailSend(const game::PlayerMailSendReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_player_mail_send();
    auto fail = [&](const char *code, const char *msg) {
        body->set_ok(false);
        body->set_error_code(code);
        body->set_message(msg);
        rsp->set_ok(false);
        rsp->set_message(msg);
        return false;
    };
    if (!ready_ && !Init())
        return fail(mail::err::kInternal, "mail service not ready");
    // V1 只投系统/活动邮件。玩家寄信留在这条入口，正式环境关闭。
    if (FormalModeEnabled())
        return fail("ERR_COMMAND_FORBIDDEN", "player mail is not open");
    const uint64_t sender = req.sender_player_id();
    const uint64_t receiver = req.receiver_player_id();
    if (sender == 0 || receiver == 0)
        return fail("ERR_INVALID_ARGUMENT", "sender and receiver required");
    if (sender == receiver)
        return fail("ERR_MAIL_SELF", "cannot mail self");
    if (req.operation_id().empty())
        return fail("ERR_INVALID_ARGUMENT", "operation_id required");
    std::string tec;
    if (!utf8text::ValidBoundedText(req.title(), 1, 64, false, &tec))
        return fail(tec.c_str(), "invalid title");
    if (!utf8text::ValidBoundedText(req.body(), 1, 1000, true, &tec))
        return fail(tec.c_str(), "invalid body");
    if (!PlayerAccountStore::Instance().Exists(receiver))
        return fail("ERR_RECEIVER_NOT_FOUND", "receiver does not exist");
    const int limit = MailConfig::Instance().Values().player_mail_per_minute;
    if (!ConsumePlayerMailQuota(sender, limit, 60))
        return fail(mail::err::kRateLimited, "player mail rate limited");

    std::string sender_name = "player";
    PlayerProfileRow prow;
    std::string perr;
    if (PlayerProfileStore::Instance().Load(sender, &prow, &perr) && prow.exists &&
        !prow.player_name.empty())
        sender_name = prow.player_name;

    mail::DeliverRequest d;
    d.source_system = "player_mail";
    d.business_key = req.operation_id();
    d.receiver_type = "ROLE";
    d.receiver_id = receiver;
    d.category = "SOCIAL";
    d.sender_name = sender_name;
    d.sender_type = "PLAYER";
    d.sender_id = sender;
    d.title = req.title();
    d.body = req.body();
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
    if (ok) {
        BumpVersion(receiver);
        BestEffortMailboxChanged(receiver);
    }
    return ok;
}

int MailService::ScanExpire(int limit) {
    if (!ready_ && !Init())
        return 0;
    int n = 0;
    MailStore::Instance().MarkExpiredBatch(NowUtc(), limit, &n);
    return n;
}
