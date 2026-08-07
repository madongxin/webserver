#include "AsyncMysqlGameDbRepository.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "GameDbOutbox.h"
#include "GameDbRepository.h"
#include "Logging.h"
#include "MailStore.h"
#include "MailTypes.h"
#include "NatsThinClient.h"
#include "PlayerItemStore.h"

#include <chrono>
#include <future>
#include <sstream>

namespace {

int64_t NowUtc() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

void FillFail(GameDbMailClaimResult *r, const char *code, const char *msg) {
    r->ok = false;
    r->should_apply_memory = false;
    r->error_code = code;
    r->message = msg;
}

}  // namespace

AsyncMysqlGameDbRepository &AsyncMysqlGameDbRepository::Instance() {
    static AsyncMysqlGameDbRepository g;
    return g;
}

AsyncMysqlGameDbRepository::~AsyncMysqlGameDbRepository() { Stop(); }

void AsyncMysqlGameDbRepository::Start(int worker_count) {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (started_)
        return;
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        LOG_WARN << "GameDB: MySQL pool not ready, skip Start";
        return;
    }
    if (!GameDbOutbox::Instance().EnsureTable()) {
        LOG_WARN << "GameDB: outbox EnsureTable failed";
        return;
    }
    if (!PlayerItemStore::Instance().EnsureTable()) {
        LOG_WARN << "GameDB: player_item EnsureTable failed";
        return;
    }
    stop_ = false;
    if (worker_count < 1)
        worker_count = 1;
    for (int i = 0; i < worker_count; ++i)
        workers_.emplace_back([this] { WorkerLoop(); });
    publisher_ = std::thread([this] { PublisherLoop(); });
    started_ = true;
    GameDbRepository::Set(this);
    LOG_INFO << "GameDB: started workers=" << worker_count;
}

void AsyncMysqlGameDbRepository::Stop() {
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        if (!started_)
            return;
        stop_ = true;
    }
    q_cv_.notify_all();
    pub_cv_.notify_all();
    for (auto &t : workers_) {
        if (t.joinable())
            t.join();
    }
    workers_.clear();
    if (publisher_.joinable())
        publisher_.join();
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        started_ = false;
    }
    if (GameDbRepository::Get() == this)
        GameDbRepository::Set(nullptr);
    LOG_INFO << "GameDB: stopped";
}

void AsyncMysqlGameDbRepository::ClaimMailAttachmentsAsync(GameDbMailClaimRequest req,
                                                           MailClaimDone done) {
    if (!done)
        return;
    if (!started_) {
        GameDbMailClaimResult r;
        FillFail(&r, mail::err::kInternal, "gamedb not started");
        done(std::move(r));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        q_.push_back(Job{std::move(req), std::move(done)});
    }
    q_cv_.notify_one();
}

GameDbMailClaimResult AsyncMysqlGameDbRepository::ClaimMailAttachments(GameDbMailClaimRequest req) {
    auto prom = std::make_shared<std::promise<GameDbMailClaimResult>>();
    auto fut = prom->get_future();
    ClaimMailAttachmentsAsync(std::move(req), [prom](GameDbMailClaimResult r) {
        prom->set_value(std::move(r));
    });
    return fut.get();
}

void AsyncMysqlGameDbRepository::WorkerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty())
                return;
            job = std::move(q_.front());
            q_.pop_front();
        }
        GameDbMailClaimResult result = DoClaimMail(job.req);
        if (job.done)
            job.done(std::move(result));
    }
}

void AsyncMysqlGameDbRepository::PublisherLoop() {
    while (!stop_) {
        PublishBatch(100);
        std::unique_lock<std::mutex> lk(pub_mu_);
        pub_cv_.wait_for(lk, std::chrono::seconds(1), [this] { return stop_.load(); });
    }
}

void AsyncMysqlGameDbRepository::SetNatsUrl(std::string url) {
    NatsThinClient::Instance().SetUrl(std::move(url));
}

void AsyncMysqlGameDbRepository::PublishBatch(int limit) {
    std::vector<GameDbOutboxRow> rows;
    if (!GameDbOutbox::Instance().FetchUnpublished(limit, &rows))
        return;
    const int64_t now = NowUtc();
    for (const auto &r : rows) {
        LOG_INFO << "GameDB outbox publish id=" << r.id << " type=" << r.event_type
                 << " aggregate=" << r.aggregate_type << "/" << r.aggregate_id
                 << " idem=" << r.idempotency_key;
        bool published = true;
        if (NatsThinClient::Instance().enabled()) {
            const std::string subject = "gamedb." + r.event_type;
            published = NatsThinClient::Instance().Publish(subject, r.payload);
            if (!published) {
                LOG_WARN << "GameDB outbox NATS publish failed id=" << r.id << " keep unpublished";
                continue;
            }
        }
        if (published)
            GameDbOutbox::Instance().MarkPublished(r.id, now);
    }
}

int AsyncMysqlGameDbRepository::PublishOnceForTest(int limit) {
    std::vector<GameDbOutboxRow> rows;
    if (!GameDbOutbox::Instance().FetchUnpublished(limit, &rows))
        return -1;
    const int64_t now = NowUtc();
    int n = 0;
    for (const auto &r : rows) {
        if (GameDbOutbox::Instance().MarkPublished(r.id, now))
            ++n;
    }
    return n;
}

GameDbMailClaimResult AsyncMysqlGameDbRepository::DoClaimMail(const GameDbMailClaimRequest &req) {
    GameDbMailClaimResult result;
    result.ok = false;
    result.should_apply_memory = false;

    if (req.idempotency_key.empty()) {
        FillFail(&result, mail::err::kInvalidArgument, "idempotency_key required");
        return result;
    }

    MailOpLogRow existed;
    if (MailStore::Instance().FindOpByIdempotency(req.idempotency_key, &existed)) {
        result.idempotent_hit = true;
        result.ok = (existed.result_code == mail::err::kOk ||
                     existed.result_code == mail::err::kAlreadyClaimed);
        result.error_code = existed.result_code;
        result.message = "idempotent";
        result.attachment_state =
            (existed.result_code == mail::err::kOk ||
             existed.result_code == mail::err::kAlreadyClaimed)
                ? "CLAIMED"
                : "";
        result.should_apply_memory = false;
        return result;
    }

    auto conn = MailStore::Instance().GetConnection();
    if (!conn) {
        FillFail(&result, mail::err::kInternal, "db");
        return result;
    }
    const int64_t now = NowUtc();
    if (!MailStore::Instance().Begin(conn.get())) {
        FillFail(&result, mail::err::kInternal, "begin");
        return result;
    }

    mail::MailInstanceRow row;
    if (!MailStore::Instance().LoadMailForUpdate(conn.get(), req.mail_id, &row)) {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kMailNotFound, "not found");
        return result;
    }
    result.mail_row_version = row.row_version;

    if (row.receiver_id != req.player_id) {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kPermissionDenied, "denied");
        return result;
    }
    if (mail::IsExpiredAt(row.expire_at, now) || row.visible_state == "EXPIRED") {
        row.visible_state = "EXPIRED";
        row.updated_at = now;
        row.row_version += 1;
        MailStore::Instance().UpdateMailRow(conn.get(), row);
        MailStore::Instance().Commit(conn.get());
        FillFail(&result, mail::err::kMailExpired, "expired");
        result.mail_row_version = row.row_version;
        return result;
    }
    if (row.visible_state == "REVOKED") {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kMailRevoked, "revoked");
        return result;
    }
    if (row.visible_state != "ACTIVE") {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kInvalidState, "not active");
        return result;
    }
    if (row.attachment_state == "CLAIMED") {
        // 已领取：尽量写幂等日志；冲突则读已有记录（并发同 key）
        MailOpLogRow op;
        op.mail_id = req.mail_id;
        op.actor_id = req.player_id;
        op.operation_type = "CLAIM";
        op.idempotency_key = req.idempotency_key;
        op.before_state = "{\"attachment\":\"CLAIMED\"}";
        op.after_state = "{\"attachment\":\"CLAIMED\"}";
        op.result_code = mail::err::kAlreadyClaimed;
        op.trace_id = req.trace_id;
        op.created_at = now;
        const bool logged = MailStore::Instance().InsertOpLog(conn.get(), op);
        if (logged) {
            if (!MailStore::Instance().Commit(conn.get())) {
                MailStore::Instance().Rollback(conn.get());
                FillFail(&result, mail::err::kInternal, "already-claimed commit");
                return result;
            }
            result.ok = true;
            result.error_code = mail::err::kAlreadyClaimed;
            result.message = "already claimed";
            result.attachment_state = "CLAIMED";
            result.mail_row_version = row.row_version;
            return result;
        }
        MailStore::Instance().Rollback(conn.get());
        if (MailStore::Instance().FindOpByIdempotency(req.idempotency_key, &existed)) {
            result.idempotent_hit = true;
            result.ok = true;
            result.error_code = existed.result_code;
            result.message = "idempotent";
            result.attachment_state = "CLAIMED";
            return result;
        }
        // 他线程已领完但本 key 未落日志：仍视为已领取成功（资产只入账一次）
        result.ok = true;
        result.error_code = mail::err::kAlreadyClaimed;
        result.message = "already claimed";
        result.attachment_state = "CLAIMED";
        result.mail_row_version = row.row_version;
        return result;
    }
    if (row.attachment_state == "CLAIMING") {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kClaimInProgress, "claiming");
        return result;
    }
    if (row.attachment_state != "UNCLAIMED") {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kInvalidAttachment, "no claimable attachment");
        return result;
    }
    if (!mail::CanTransitAttachment(mail::AttachmentState::kUnclaimed,
                                    mail::AttachmentState::kClaiming)) {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kInvalidState, "illegal transition");
        return result;
    }

    const int64_t version_before = row.row_version;
    row.attachment_state = "CLAIMING";
    row.updated_at = now;
    row.row_version += 1;
    if (!MailStore::Instance().UpdateMailRow(conn.get(), row) ||
        !MailStore::Instance().UpdateAttachmentsClaimState(conn.get(), req.mail_id, "CLAIMING")) {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kInternal, "mark claiming failed");
        return result;
    }

    std::vector<mail::MailAttachmentRow> atts;
    if (!MailStore::Instance().LoadAttachmentsForUpdate(conn.get(), req.mail_id, &atts) ||
        atts.empty()) {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kInvalidAttachment, "attachments missing");
        return result;
    }

    for (const auto &a : atts) {
        if (a.asset_type != "ITEM" || a.asset_id == 0 || a.count == 0) {
            MailStore::Instance().Rollback(conn.get());
            FillFail(&result, mail::err::kInvalidAttachment, "invalid asset");
            return result;
        }
        if (a.asset_id > 0xFFFFFFFFu) {
            MailStore::Instance().Rollback(conn.get());
            FillFail(&result, mail::err::kInvalidAttachment, "asset_id too large");
            return result;
        }
        const uint32_t item_id = static_cast<uint32_t>(a.asset_id);
        uint32_t cur = 0;
        auto it = req.bag_snapshot.find(item_id);
        if (it != req.bag_snapshot.end())
            cur = it->second;
        if (static_cast<int64_t>(cur) + a.count > req.inventory_soft_cap) {
            MailStore::Instance().Rollback(conn.get());
            FillFail(&result, mail::err::kInventoryFull, "inventory soft cap");
            return result;
        }
    }

    std::ostringstream tx;
    tx << "mail:" << req.mail_id << ":" << now;
    const std::string asset_tx = tx.str();
    std::vector<GameDbGrantedItem> grants;
    for (const auto &a : atts) {
        uint64_t instance_id = 0;
        std::ostringstream extra;
        extra << "{\"source\":\"mail_claim\",\"mail_id\":" << req.mail_id << ",\"tx\":\""
              << asset_tx << "\"}";
        if (!PlayerItemStore::Instance().InsertOnConnection(conn.get(), req.player_id, a.asset_id,
                                                            a.count, 0, extra.str(), &instance_id)) {
            MailStore::Instance().Rollback(conn.get());
            FillFail(&result, mail::err::kAssetTransactionFailed, "asset insert failed");
            return result;
        }
        GameDbGrantedItem g;
        g.asset_id = a.asset_id;
        g.count = static_cast<uint32_t>(a.count);
        grants.push_back(g);
    }

    row.attachment_state = "CLAIMED";
    if (row.read_state != "READ") {
        row.read_state = "READ";
        row.read_at = now;
    }
    row.updated_at = now;
    row.row_version += 1;
    if (!MailStore::Instance().UpdateMailRow(conn.get(), row) ||
        !MailStore::Instance().UpdateAttachmentsClaimed(conn.get(), req.mail_id, asset_tx, now)) {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kInternal, "finalize claim failed");
        return result;
    }

    MailOpLogRow op;
    op.mail_id = req.mail_id;
    op.actor_id = req.player_id;
    op.operation_type = "CLAIM";
    op.idempotency_key = req.idempotency_key;
    op.before_state = "{\"attachment\":\"UNCLAIMED\",\"row_version\":" +
                      std::to_string(version_before) + "}";
    op.after_state = "{\"attachment\":\"CLAIMED\",\"tx\":\"" + asset_tx +
                     "\",\"row_version\":" + std::to_string(row.row_version) + "}";
    op.result_code = mail::err::kOk;
    op.trace_id = req.trace_id;
    op.created_at = now;
    if (!MailStore::Instance().InsertOpLog(conn.get(), op)) {
        MailStore::Instance().Rollback(conn.get());
        if (MailStore::Instance().FindOpByIdempotency(req.idempotency_key, &existed)) {
            result.idempotent_hit = true;
            result.ok = (existed.result_code == mail::err::kOk ||
                         existed.result_code == mail::err::kAlreadyClaimed);
            result.error_code = existed.result_code;
            result.message = "idempotent";
            result.attachment_state = "CLAIMED";
            result.should_apply_memory = false;
            return result;
        }
        FillFail(&result, mail::err::kInternal, "op log failed");
        return result;
    }

    std::ostringstream payload;
    payload << "{\"mail_id\":" << req.mail_id << ",\"player_id\":" << req.player_id
            << ",\"tx\":\"" << asset_tx << "\",\"grants\":[";
    for (size_t i = 0; i < grants.size(); ++i) {
        if (i)
            payload << ",";
        payload << "{\"asset_id\":" << grants[i].asset_id << ",\"count\":" << grants[i].count
                << "}";
    }
    payload << "]}";
    if (!GameDbOutbox::Instance().InsertOnConnection(
            conn.get(), "mail.claimed", "mail", std::to_string(req.mail_id), req.idempotency_key,
            payload.str(), now)) {
        MailStore::Instance().Rollback(conn.get());
        if (MailStore::Instance().FindOpByIdempotency(req.idempotency_key, &existed)) {
            result.idempotent_hit = true;
            result.ok = true;
            result.error_code = existed.result_code;
            result.message = "idempotent";
            result.attachment_state = "CLAIMED";
            return result;
        }
        FillFail(&result, mail::err::kInternal, "outbox insert failed");
        return result;
    }

    if (!MailStore::Instance().Commit(conn.get())) {
        MailStore::Instance().Rollback(conn.get());
        FillFail(&result, mail::err::kInternal, "commit failed");
        return result;
    }

    result.ok = true;
    result.error_code = mail::err::kOk;
    result.message = "claimed";
    result.attachment_state = "CLAIMED";
    result.mail_row_version = row.row_version;
    result.grants = std::move(grants);
    result.should_apply_memory = true;
    result.idempotent_hit = false;
    return result;
}
