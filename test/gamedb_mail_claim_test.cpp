/**
 * @file gamedb_mail_claim_test.cpp
 * @brief 阶段 4 / 稳定评估阶段1：邮件领取写入正式 player_asset_bag + 幂等/崩溃重试
 *
 * 运行：./build/test/gamedb_mail_claim_test
 */

#include "AsyncMysqlGameDbRepository.h"
#include "ConnectionPool.h"
#include "GameDbAssetStore.h"
#include "GameDbOutbox.h"
#include "GameLogic.h"
#include "Logging.h"
#include "MailService.h"
#include "MailStore.h"
#include "MailTypes.h"
#include "game.pb.h"

#include <mysql/mysql.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;

#define EXPECT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint32_t BagCount(uint64_t player_id, uint32_t item_id) {
    std::map<uint32_t, uint32_t> bag;
    uint64_t ver = 0;
    if (!GameDbAssetStore::Instance().LoadInventory(player_id, &bag, &ver))
        return 0;
    auto it = bag.find(item_id);
    return it == bag.end() ? 0 : it->second;
}

uint64_t BagVersion(uint64_t player_id) {
    uint64_t ver = 0;
    bool exists = false;
    if (!GameDbAssetStore::Instance().LoadMeta(player_id, &ver, &exists) || !exists)
        return 0;
    return ver;
}

uint64_t DeliverTestMail(uint64_t player_id, uint32_t item_id, int count, const std::string &biz) {
    mail::DeliverRequest d;
    d.source_system = "gamedb_test";
    d.business_key = biz;
    d.receiver_type = "ROLE";
    d.receiver_id = player_id;
    d.category = "SYSTEM";
    d.title = "gamedb claim";
    d.body = "phase1_asset";
    d.sender_name = "Test";
    mail::DeliverAttachment a;
    a.asset_type = "ITEM";
    a.asset_id = item_id;
    a.count = count;
    d.attachments.push_back(a);
    uint64_t mail_id = 0;
    std::string ec, msg;
    if (!MailService::Instance().Deliver(d, &mail_id, &ec, &msg))
        return 0;
    return mail_id;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);

    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        std::fprintf(stderr, "FAIL: MySQL pool not initialized (config/mysql.cnf)\n");
        return 1;
    }
    if (!MailService::Instance().Init()) {
        std::fprintf(stderr, "FAIL: MailService::Init\n");
        return 1;
    }
    EXPECT_TRUE(GameDbAssetStore::Instance().EnsureTables());
    EXPECT_TRUE(AsyncMysqlGameDbRepository::Instance().started());

    const uint32_t item_id = 2101;
    const uint32_t seed_item = 2102;

    // ---------- A: 首次领取写入正式 bag + outbox + 幂等重试不重复入账 ----------
    {
        const uint64_t player_id = 910001;
        // 对齐内存与正式 bag，避免旧 player_item 脏内存干扰版本衔接判定
        {
            std::map<uint32_t, uint32_t> bag0;
            uint64_t ver0 = 0;
            (void)GameDbAssetStore::Instance().LoadInventory(player_id, &bag0, &ver0);
            std::map<uint32_t, int64_t> cds;
            GameLogic::Instance().ImportRuntimeState(player_id, bag0, cds, ver0 == 0 ? 1 : ver0);
        }
        const std::string biz = "gamedb_idem_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(player_id, item_id, 5, biz);
        EXPECT_TRUE(mail_id > 0);
        if (mail_id == 0)
            return 1;

        const std::string idem = "gamedb_claim_" + std::to_string(mail_id);
        const uint32_t bag_before = BagCount(player_id, item_id);
        const uint32_t mem_before = GameLogic::Instance().GetItemCount(player_id, item_id);
        const uint64_t ver_before = BagVersion(player_id);
        EXPECT_TRUE(mem_before == bag_before);
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(player_id) ==
                    (ver_before == 0 ? 1 : ver_before));

        game::GameResponse rsp1;
        game::MailClaimReq req1;
        req1.set_player_id(player_id);
        req1.set_mail_id(mail_id);
        req1.set_idempotency_key(idem);
        req1.set_trace_id("t1");
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req1, &rsp1));
        EXPECT_TRUE(rsp1.ok());
        EXPECT_TRUE(rsp1.mail_claim().result().error_code() == mail::err::kOk);
        EXPECT_TRUE(BagCount(player_id, item_id) == bag_before + 5);
        const uint64_t ver_after = BagVersion(player_id);
        EXPECT_TRUE(ver_after == (ver_before == 0 ? 2 : ver_before + 1));
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency(idem) == 1);
        EXPECT_TRUE(GameLogic::Instance().GetItemCount(player_id, item_id) == mem_before + 5);
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(player_id) == ver_after);

        const uint32_t bag_mid = BagCount(player_id, item_id);
        game::GameResponse rsp2;
        game::MailClaimReq req2 = req1;
        req2.set_trace_id("t2");
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req2, &rsp2));
        EXPECT_TRUE(rsp2.ok());
        EXPECT_TRUE(BagCount(player_id, item_id) == bag_mid);
        EXPECT_TRUE(BagVersion(player_id) == ver_after);
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(player_id) == ver_after);
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency(idem) == 1);

        const int published = AsyncMysqlGameDbRepository::Instance().PublishOnceForTest(10);
        EXPECT_TRUE(published >= 0);  // NATS 可选；Outbox 行已在上面断言
        (void)published;
    }

    // ---------- A2: 已有正式版本 → 领邮件 → Consume/SaveSnapshot 不 VERSION_CONFLICT ----------
    {
        const uint64_t pid = 910010;
        GameDbAssetStore::MutationResult seed;
        const std::string seed_key = "mail_ver_seed_" + std::to_string(NowMs());
        EXPECT_TRUE(GameDbAssetStore::Instance().ApplyMutation(pid, seed_key, 0, "GRANT", seed_item,
                                                               3, &seed) &&
                    seed.ok);
        // 对齐内存版本与背包（模拟 Bind 加载）
        std::map<uint32_t, uint32_t> bag;
        uint64_t ver0 = 0;
        EXPECT_TRUE(GameDbAssetStore::Instance().LoadInventory(pid, &bag, &ver0));
        std::map<uint32_t, int64_t> cds;
        EXPECT_TRUE(GameLogic::Instance().ImportRuntimeState(pid, bag, cds, ver0));
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(pid) == ver0);

        const std::string biz = "gamedb_ver_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 2, biz);
        EXPECT_TRUE(mail_id > 0);
        const std::string idem = "gamedb_ver_" + std::to_string(mail_id);
        game::GameResponse rsp;
        game::MailClaimReq req;
        req.set_player_id(pid);
        req.set_mail_id(mail_id);
        req.set_idempotency_key(idem);
        req.set_trace_id("ver1");
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(rsp.ok());
        const uint64_t ver1 = BagVersion(pid);
        EXPECT_TRUE(ver1 == ver0 + 1);
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(pid) == ver1);

        GameDbAssetStore::MutationResult cons;
        EXPECT_TRUE(GameDbAssetStore::Instance().ApplyMutation(
                        pid, "mail_consume_" + std::to_string(mail_id), ver1, "CONSUME", item_id, 1,
                        &cons) &&
                    cons.ok);
        EXPECT_TRUE(cons.error_code != "VERSION_CONFLICT");
        EXPECT_TRUE(cons.asset_version == ver1 + 1);
        {
            std::map<uint32_t, uint32_t> b2;
            uint64_t v2 = 0;
            EXPECT_TRUE(GameDbAssetStore::Instance().LoadInventory(pid, &b2, &v2));
            EXPECT_TRUE(GameLogic::Instance().ImportRuntimeState(pid, b2, cds, v2));
        }

        std::map<uint32_t, uint32_t> snap_bag;
        uint64_t snap_ver = 0;
        EXPECT_TRUE(GameDbAssetStore::Instance().LoadInventory(pid, &snap_bag, &snap_ver));
        GameDbAssetStore::SnapshotResult sr;
        EXPECT_TRUE(GameDbAssetStore::Instance().SaveSnapshot(
            pid, snap_ver, snap_bag, "mail_snap_" + std::to_string(mail_id), &sr));
        EXPECT_TRUE(sr.ok);
        EXPECT_TRUE(sr.error_code != "VERSION_CONFLICT");
    }

    // ---------- B: commit 后不 Apply 内存再重试（崩溃窗口）----------
    {
        const uint64_t pid = 910002;
        const std::string biz = "gamedb_crash_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 2, biz);
        EXPECT_TRUE(mail_id > 0);
        const std::string idem = "gamedb_crash_" + std::to_string(mail_id);
        const uint32_t before = BagCount(pid, item_id);

        GameDbMailClaimRequest db_req;
        db_req.player_id = pid;
        db_req.mail_id = mail_id;
        db_req.idempotency_key = idem;
        db_req.trace_id = "crash1";
        GameLogic::Instance().CopyInventory(pid, &db_req.bag_snapshot);
        auto first = AsyncMysqlGameDbRepository::Instance().ClaimMailAttachments(db_req);
        EXPECT_TRUE(first.ok);
        EXPECT_TRUE(first.should_apply_memory);
        EXPECT_TRUE(first.asset_version > 0);
        EXPECT_TRUE(BagCount(pid, item_id) == before + 2);
        const uint64_t claimed_ver = first.asset_version;

        auto second = AsyncMysqlGameDbRepository::Instance().ClaimMailAttachments(db_req);
        EXPECT_TRUE(second.ok);
        EXPECT_TRUE(second.idempotent_hit);
        EXPECT_TRUE(!second.should_apply_memory);
        EXPECT_TRUE(second.asset_version == claimed_ver);
        EXPECT_TRUE(BagCount(pid, item_id) == before + 2);
        EXPECT_TRUE(BagVersion(pid) == claimed_ver);
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency(idem) == 1);

        // 模拟响应丢失后补 Apply：版本对齐且不重复加道具
        EXPECT_TRUE(GameLogic::Instance().ApplyItemRewardsWithVersion(pid, first.grants,
                                                                      first.asset_version));
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(pid) == claimed_ver);
    }

    // ---------- B2: 批量领取多封，版本连续 ----------
    {
        const uint64_t pid = 910011;
        std::map<uint32_t, uint32_t> bag;
        uint64_t ver = 0;
        (void)GameDbAssetStore::Instance().LoadInventory(pid, &bag, &ver);
        std::map<uint32_t, int64_t> cds;
        GameLogic::Instance().ImportRuntimeState(pid, bag, cds, ver == 0 ? 1 : ver);
        uint64_t cur = GameLogic::Instance().GetAssetVersion(pid);
        for (int i = 0; i < 3; ++i) {
            const std::string biz = "gamedb_batch_" + std::to_string(NowMs()) + "_" + std::to_string(i);
            const uint64_t mail_id = DeliverTestMail(pid, item_id, 1, biz);
            EXPECT_TRUE(mail_id > 0);
            game::GameResponse rsp;
            game::MailClaimReq req;
            req.set_player_id(pid);
            req.set_mail_id(mail_id);
            req.set_idempotency_key("batch_" + std::to_string(mail_id));
            req.set_trace_id("b" + std::to_string(i));
            EXPECT_TRUE(MailService::Instance().HandleMailClaim(req, &rsp));
            EXPECT_TRUE(rsp.ok());
            const uint64_t dbv = BagVersion(pid);
            EXPECT_TRUE(dbv == cur + 1);
            EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(pid) == dbv);
            cur = dbv;
        }
    }

    // ---------- C: 已有正式背包 → 领邮件 → 模拟 kill/relogin 只从 bag 恢复 ----------
    {
        const uint64_t pid = 910004;
        GameDbAssetStore::MutationResult seed;
        const std::string seed_key = "mail_seed_" + std::to_string(NowMs());
        EXPECT_TRUE(GameDbAssetStore::Instance().ApplyMutation(pid, seed_key, 0, "GRANT", seed_item,
                                                               7, &seed) &&
                    seed.ok);
        const uint32_t seed_before = BagCount(pid, seed_item);
        EXPECT_TRUE(seed_before >= 7);

        const std::string biz = "gamedb_relogin_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 4, biz);
        EXPECT_TRUE(mail_id > 0);
        const std::string idem = "gamedb_relogin_" + std::to_string(mail_id);
        const uint32_t reward_before = BagCount(pid, item_id);

        GameDbMailClaimRequest db_req;
        db_req.player_id = pid;
        db_req.mail_id = mail_id;
        db_req.idempotency_key = idem;
        db_req.trace_id = "relogin1";
        auto first = AsyncMysqlGameDbRepository::Instance().ClaimMailAttachments(db_req);
        EXPECT_TRUE(first.ok);
        EXPECT_TRUE(first.should_apply_memory);
        EXPECT_TRUE(BagCount(pid, item_id) == reward_before + 4);
        EXPECT_TRUE(BagCount(pid, seed_item) == seed_before);

        // 模拟 GameLogic 崩溃：不 ApplyClaimMemory；重登只读正式 bag
        std::map<uint32_t, uint32_t> reloaded;
        uint64_t ver = 0;
        EXPECT_TRUE(GameDbAssetStore::Instance().LoadInventory(pid, &reloaded, &ver));
        EXPECT_TRUE(reloaded[item_id] == reward_before + 4);
        EXPECT_TRUE(reloaded[seed_item] == seed_before);

        auto second = AsyncMysqlGameDbRepository::Instance().ClaimMailAttachments(db_req);
        EXPECT_TRUE(second.ok);
        EXPECT_TRUE(second.idempotent_hit);
        EXPECT_TRUE(!second.should_apply_memory);
        EXPECT_TRUE(second.asset_version == BagVersion(pid));
        EXPECT_TRUE(BagCount(pid, item_id) == reward_before + 4);
    }

    // ---------- D: 并发同 key ----------
    {
        const uint64_t pid = 910003;
        const std::string biz = "gamedb_conc_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 3, biz);
        EXPECT_TRUE(mail_id > 0);
        const std::string idem = "gamedb_conc_" + std::to_string(mail_id);
        const uint32_t before = BagCount(pid, item_id);
        std::atomic<int> ok_n{0};
        std::vector<std::thread> ths;
        for (int i = 0; i < 8; ++i) {
            ths.emplace_back([&, i] {
                game::GameResponse rsp;
                game::MailClaimReq req;
                req.set_player_id(pid);
                req.set_mail_id(mail_id);
                req.set_idempotency_key(idem);
                req.set_trace_id("c" + std::to_string(i));
                if (MailService::Instance().HandleMailClaim(req, &rsp) && rsp.ok())
                    ok_n.fetch_add(1);
            });
        }
        for (auto &t : ths)
            t.join();
        EXPECT_TRUE(ok_n.load() >= 1);
        EXPECT_TRUE(BagCount(pid, item_id) == before + 3);
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency(idem) == 1);
    }

    // ---------- E: 内存应用失败 + 权威重载成功 ----------
    {
        const uint64_t pid = 910020;
        std::map<uint32_t, uint32_t> bag;
        uint64_t ver = 0;
        (void)GameDbAssetStore::Instance().LoadInventory(pid, &bag, &ver);
        std::map<uint32_t, int64_t> cds;
        GameLogic::Instance().ImportRuntimeState(pid, bag, cds, ver == 0 ? 1 : ver);
        GameLogic::Instance().SetAssetApplyBlockedForTest(true);
        const uint32_t before = GameLogic::Instance().GetItemCount(pid, item_id);
        const std::string biz = "gamedb_reload_ok_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 2, biz);
        EXPECT_TRUE(mail_id > 0);
        game::GameResponse rsp;
        game::MailClaimReq req;
        req.set_player_id(pid);
        req.set_mail_id(mail_id);
        req.set_idempotency_key("reload_ok_" + std::to_string(mail_id));
        req.set_trace_id("reload_ok");
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(rsp.ok());
        EXPECT_TRUE(GameLogic::Instance().GetItemCount(pid, item_id) == before + 2);
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(pid) == BagVersion(pid));
        EXPECT_TRUE(!GameLogic::Instance().IsAssetDirty(pid));
        GameLogic::Instance().SetAssetApplyBlockedForTest(false);
    }

    // ---------- F: 应用与重载都失败 → STATE_SYNC_REQUIRED，拒绝资产写；同键重试不重复发奖 ----------
    {
        const uint64_t pid = 910021;
        std::map<uint32_t, uint32_t> bag;
        uint64_t ver = 0;
        (void)GameDbAssetStore::Instance().LoadInventory(pid, &bag, &ver);
        std::map<uint32_t, int64_t> cds;
        GameLogic::Instance().ImportRuntimeState(pid, bag, cds, ver == 0 ? 1 : ver);
        const uint32_t db_before = BagCount(pid, item_id);
        const uint32_t mem_before = GameLogic::Instance().GetItemCount(pid, item_id);
        const std::string biz = "gamedb_sync_req_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 3, biz);
        EXPECT_TRUE(mail_id > 0);
        const std::string idem = "sync_req_" + std::to_string(mail_id);
        GameLogic::Instance().SetAssetApplyBlockedForTest(true);
        GameLogic::Instance().SetAssetReloadBlockedForTest(true);
        game::GameResponse rsp;
        game::MailClaimReq req;
        req.set_player_id(pid);
        req.set_mail_id(mail_id);
        req.set_idempotency_key(idem);
        req.set_trace_id("sync1");
        EXPECT_TRUE(!MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(rsp.mail_claim().result().error_code() == mail::err::kStateSyncRequired);
        EXPECT_TRUE(BagCount(pid, item_id) == db_before + 3);
        EXPECT_TRUE(GameLogic::Instance().GetItemCount(pid, item_id) == mem_before);
        EXPECT_TRUE(GameLogic::Instance().IsAssetDirty(pid));

        game::GameResponse crsp;
        game::ConsumeItemReq creq;
        creq.set_player_id(pid);
        creq.set_item_id(item_id);
        creq.set_count(1);
        EXPECT_TRUE(!GameLogic::Instance().HandleConsumeItemForTest(creq, &crsp));
        EXPECT_TRUE(crsp.message() == "STATE_SYNC_REQUIRED");

        game::GameResponse rsp2;
        EXPECT_TRUE(!MailService::Instance().HandleMailClaim(req, &rsp2));
        EXPECT_TRUE(rsp2.mail_claim().result().error_code() == mail::err::kStateSyncRequired);
        EXPECT_TRUE(BagCount(pid, item_id) == db_before + 3);

        GameLogic::Instance().SetAssetReloadBlockedForTest(false);
        GameLogic::Instance().SetAssetApplyBlockedForTest(false);
        game::GameResponse rsp3;
        const bool recovered = MailService::Instance().HandleMailClaim(req, &rsp3);
        EXPECT_TRUE(recovered || rsp3.mail_claim().result().error_code() == mail::err::kAlreadyClaimed);
        EXPECT_TRUE(BagCount(pid, item_id) == db_before + 3);
        EXPECT_TRUE(GameLogic::Instance().GetItemCount(pid, item_id) == db_before + 3);
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(pid) == BagVersion(pid));
        EXPECT_TRUE(!GameLogic::Instance().IsAssetDirty(pid));
    }

    // ---------- G: 同一封邮件换 idempotency key，只入账一次 ----------
    {
        const uint64_t pid = 910042;
        const std::string biz = "gamedb_diffkey_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 4, biz);
        EXPECT_TRUE(mail_id > 0);
        const uint32_t before = BagCount(pid, item_id);
        game::MailClaimReq req;
        req.set_player_id(pid);
        req.set_mail_id(mail_id);
        req.set_idempotency_key("diff_a_" + std::to_string(mail_id));
        req.set_trace_id("diff_a");
        game::GameResponse rsp;
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(rsp.ok());
        req.set_idempotency_key("diff_b_" + std::to_string(mail_id));
        req.set_trace_id("diff_b");
        game::GameResponse rsp2;
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req, &rsp2));
        EXPECT_TRUE(rsp2.mail_claim().result().error_code() == mail::err::kAlreadyClaimed);
        EXPECT_TRUE(rsp2.mail_claim().result().attachment_state() == "CLAIMED");
        EXPECT_TRUE(BagCount(pid, item_id) == before + 4);
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency("diff_a_" + std::to_string(mail_id)) ==
                    1);
    }

    // ---------- H: 不同 key 并发领取同一封 ----------
    {
        const uint64_t pid = 910043;
        const std::string biz = "gamedb_racekey_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 6, biz);
        EXPECT_TRUE(mail_id > 0);
        const uint32_t before = BagCount(pid, item_id);
        std::vector<std::thread> ths;
        for (int i = 0; i < 4; ++i) {
            ths.emplace_back([pid, mail_id, i] {
                game::MailClaimReq req;
                req.set_player_id(pid);
                req.set_mail_id(mail_id);
                req.set_idempotency_key("race_" + std::to_string(i) + "_" + std::to_string(mail_id));
                req.set_trace_id("race" + std::to_string(i));
                game::GameResponse rsp;
                (void)MailService::Instance().HandleMailClaim(req, &rsp);
            });
        }
        for (auto &t : ths)
            t.join();
        EXPECT_TRUE(BagCount(pid, item_id) == before + 6);
    }

    // ---------- I: 其它角色领取被拒绝，资产不变 ----------
    {
        const uint64_t owner = 910044;
        const uint64_t other = 910045;
        const std::string biz = "gamedb_other_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(owner, item_id, 2, biz);
        EXPECT_TRUE(mail_id > 0);
        const uint32_t before = BagCount(owner, item_id);
        game::MailClaimReq req;
        req.set_player_id(other);
        req.set_mail_id(mail_id);
        req.set_idempotency_key("other_" + std::to_string(mail_id));
        req.set_trace_id("other");
        game::GameResponse rsp;
        EXPECT_TRUE(!MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(rsp.mail_claim().result().error_code() == mail::err::kPermissionDenied);
        EXPECT_TRUE(BagCount(owner, item_id) == before);
        EXPECT_TRUE(BagCount(other, item_id) == 0 || BagCount(other, item_id) == before);
    }

    // ---------- J: 幂等重试时内存版本落后一档，空增量不能把版本抬上去 ----------
    {
        const uint64_t pid = 910040;
        const std::string biz = "gamedb_replay_mem_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, item_id, 8, biz);
        EXPECT_TRUE(mail_id > 0);
        const uint32_t before = BagCount(pid, item_id);
        GameDbMailClaimRequest db_req;
        db_req.player_id = pid;
        db_req.mail_id = mail_id;
        db_req.idempotency_key = "replay_mem_" + std::to_string(mail_id);
        db_req.trace_id = "replay_mem";
        auto first = AsyncMysqlGameDbRepository::Instance().ClaimMailAttachments(db_req);
        EXPECT_TRUE(first.ok);
        EXPECT_TRUE(first.asset_version > 0);
        EXPECT_TRUE(BagCount(pid, item_id) == before + 8);
        std::map<uint32_t, uint32_t> stale;
        stale[item_id] = before;
        std::map<uint32_t, int64_t> cds;
        const uint64_t stale_ver = first.asset_version - 1;
        EXPECT_TRUE(GameLogic::Instance().ImportRuntimeState(pid, stale, cds, stale_ver == 0 ? 1 : stale_ver));
        game::MailClaimReq req;
        req.set_player_id(pid);
        req.set_mail_id(mail_id);
        req.set_idempotency_key(db_req.idempotency_key);
        req.set_trace_id("replay_mem_retry");
        game::GameResponse rsp;
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(GameLogic::Instance().GetItemCount(pid, item_id) == before + 8);
        EXPECT_TRUE(GameLogic::Instance().GetAssetVersion(pid) == BagVersion(pid));
        EXPECT_TRUE(BagCount(pid, item_id) == before + 8);
    }

    // ---------- K: 背包达到软上限，领取回滚，邮件仍未领 ----------
    {
        const uint64_t pid = 910041;
        const uint32_t full_item = 2103;
        GameDbAssetStore::MutationResult seed;
        EXPECT_TRUE(GameDbAssetStore::Instance().ApplyMutation(
            pid, "full_seed_" + std::to_string(NowMs()), 0, "GRANT", full_item, 999999, &seed));
        EXPECT_TRUE(seed.ok);
        const uint32_t seeded = BagCount(pid, full_item);
        EXPECT_TRUE(seeded >= 999999);
        const std::string biz = "gamedb_full_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(pid, full_item, 1, biz);
        EXPECT_TRUE(mail_id > 0);
        game::MailClaimReq req;
        req.set_player_id(pid);
        req.set_mail_id(mail_id);
        req.set_idempotency_key("full_" + std::to_string(mail_id));
        req.set_trace_id("full");
        game::GameResponse rsp;
        EXPECT_TRUE(!MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(rsp.mail_claim().result().error_code() == mail::err::kInventoryFull);
        EXPECT_TRUE(BagCount(pid, full_item) == seeded);
        game::MailGetReq get;
        get.set_player_id(pid);
        get.set_mail_id(mail_id);
        game::GameResponse get_rsp;
        EXPECT_TRUE(MailService::Instance().HandleMailGet(get, &get_rsp));
        EXPECT_TRUE(get_rsp.mail_get().mail().brief().attachment_state() == "UNCLAIMED");
    }

    // ---------- L: 同道具数量相加溢出，整封不入账 ----------
    {
        const uint64_t pid = 910046;
        const uint32_t big_item = 2104;
        mail::DeliverRequest d;
        d.source_system = "gamedb_test";
        d.business_key = "gamedb_overflow_" + std::to_string(NowMs());
        d.receiver_type = "ROLE";
        d.receiver_id = pid;
        d.category = "SYSTEM";
        d.title = "overflow";
        d.body = "overflow";
        d.sender_name = "Test";
        for (int i = 0; i < 3; ++i) {
            mail::DeliverAttachment a;
            a.asset_type = "ITEM";
            a.asset_id = big_item;
            a.count = 2000000000;
            d.attachments.push_back(a);
        }
        uint64_t mail_id = 0;
        std::string ec, msg;
        EXPECT_TRUE(MailService::Instance().Deliver(d, &mail_id, &ec, &msg));
        EXPECT_TRUE(mail_id > 0);
        game::MailClaimReq req;
        req.set_player_id(pid);
        req.set_mail_id(mail_id);
        req.set_idempotency_key("overflow_" + std::to_string(mail_id));
        req.set_trace_id("overflow");
        game::GameResponse rsp;
        EXPECT_TRUE(!MailService::Instance().HandleMailClaim(req, &rsp));
        EXPECT_TRUE(rsp.mail_claim().result().error_code() == mail::err::kInvalidAttachment);
        EXPECT_TRUE(BagCount(pid, big_item) == 0);
        game::MailGetReq get;
        get.set_player_id(pid);
        get.set_mail_id(mail_id);
        game::GameResponse get_rsp;
        EXPECT_TRUE(MailService::Instance().HandleMailGet(get, &get_rsp));
        EXPECT_TRUE(get_rsp.mail_get().mail().brief().attachment_state() == "UNCLAIMED");
    }

    // ---------- M: 一键领取里需要手动确认的邮件有明确结果 ----------
    {
        const uint64_t pid = 910047;
        const uint64_t plain = DeliverTestMail(pid, item_id, 1, "gamedb_batch_plain_" + std::to_string(NowMs()));
        mail::DeliverRequest d;
        d.source_system = "gamedb_test";
        d.business_key = "gamedb_manual_" + std::to_string(NowMs());
        d.receiver_type = "ROLE";
        d.receiver_id = pid;
        d.category = "SYSTEM";
        d.title = "confirm";
        d.body = "confirm";
        d.sender_name = "Test";
        mail::DeliverAttachment a;
        a.asset_type = "ITEM";
        a.asset_id = item_id;
        a.count = 9;
        a.payload = "need_confirm";
        d.attachments.push_back(a);
        uint64_t manual = 0;
        std::string ec, msg;
        EXPECT_TRUE(MailService::Instance().Deliver(d, &manual, &ec, &msg));
        EXPECT_TRUE(plain > 0 && manual > 0);
        const uint32_t before = BagCount(pid, item_id);
        game::MailBatchClaimReq req;
        req.set_player_id(pid);
        req.add_mail_ids(manual);
        req.add_mail_ids(plain);
        req.set_idempotency_key_prefix("batch_manual_" + std::to_string(NowMs()));
        req.set_trace_id("batch_manual");
        game::GameResponse rsp;
        EXPECT_TRUE(MailService::Instance().HandleMailBatchClaim(req, &rsp));
        EXPECT_TRUE(rsp.mail_batch_claim().results_size() == 2);
        bool saw_manual = false;
        bool saw_plain = false;
        for (int i = 0; i < rsp.mail_batch_claim().results_size(); ++i) {
            const auto &r = rsp.mail_batch_claim().results(i);
            if (r.mail_id() == manual) {
                saw_manual = true;
                EXPECT_TRUE(!r.ok());
                EXPECT_TRUE(r.error_code() == mail::err::kInvalidState);
                EXPECT_TRUE(r.message() == "manual confirm required");
            }
            if (r.mail_id() == plain) {
                saw_plain = true;
                EXPECT_TRUE(r.ok());
            }
        }
        EXPECT_TRUE(saw_manual && saw_plain);
        EXPECT_TRUE(BagCount(pid, item_id) == before + 1);
    }

    AsyncMysqlGameDbRepository::Instance().Stop();

    if (g_fail) {
        std::fprintf(stderr, "FAILED %d checks\n", g_fail);
        std::_Exit(1);
    }
    std::printf("OK gamedb_mail_claim_test\n");
    std::fflush(stdout);
    std::_Exit(0);
}
