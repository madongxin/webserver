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
        std::fprintf(stderr, "SKIP: MySQL pool not initialized (config/mysql.cnf)\n");
        return 0;
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
        const std::string biz = "gamedb_idem_" + std::to_string(NowMs());
        const uint64_t mail_id = DeliverTestMail(player_id, item_id, 5, biz);
        EXPECT_TRUE(mail_id > 0);
        if (mail_id == 0)
            return 1;

        const std::string idem = "gamedb_claim_" + std::to_string(mail_id);
        const uint32_t bag_before = BagCount(player_id, item_id);
        const uint32_t mem_before = GameLogic::Instance().GetItemCount(player_id, item_id);
        const uint64_t ver_before = BagVersion(player_id);

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
        EXPECT_TRUE(BagVersion(player_id) == (ver_before == 0 ? 2 : ver_before + 1));
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency(idem) == 1);
        EXPECT_TRUE(GameLogic::Instance().GetItemCount(player_id, item_id) == mem_before + 5);

        const uint32_t bag_mid = BagCount(player_id, item_id);
        game::GameResponse rsp2;
        game::MailClaimReq req2 = req1;
        req2.set_trace_id("t2");
        EXPECT_TRUE(MailService::Instance().HandleMailClaim(req2, &rsp2));
        EXPECT_TRUE(rsp2.ok());
        EXPECT_TRUE(BagCount(player_id, item_id) == bag_mid);
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency(idem) == 1);

        const int published = AsyncMysqlGameDbRepository::Instance().PublishOnceForTest(10);
        EXPECT_TRUE(published >= 1);
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
        EXPECT_TRUE(BagCount(pid, item_id) == before + 2);

        auto second = AsyncMysqlGameDbRepository::Instance().ClaimMailAttachments(db_req);
        EXPECT_TRUE(second.ok);
        EXPECT_TRUE(second.idempotent_hit);
        EXPECT_TRUE(!second.should_apply_memory);
        EXPECT_TRUE(BagCount(pid, item_id) == before + 2);
        EXPECT_TRUE(GameDbOutbox::Instance().CountByIdempotency(idem) == 1);
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

    AsyncMysqlGameDbRepository::Instance().Stop();

    if (g_fail) {
        std::fprintf(stderr, "FAILED %d checks\n", g_fail);
        std::_Exit(1);
    }
    std::printf("OK gamedb_mail_claim_test\n");
    std::fflush(stdout);
    std::_Exit(0);
}
