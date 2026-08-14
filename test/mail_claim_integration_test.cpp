/**
 * @file mail_claim_integration_test.cpp
 * @brief 同一邮件并发领取 10 次，仅允许一次资产入账（MySQL 必需）
 *
 * 运行：./build/test/mail_claim_integration_test
 * 依赖：config/mysql.cnf 可用，库可建表
 */

#include "ConnectionPool.h"
#include "GameLogic.h"
#include "Logging.h"
#include "MailService.h"
#include "MailStore.h"
#include "MailTypes.h"
#include "PlayerItemStore.h"
#include "game.pb.h"

#include <mysql/mysql.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

int CountMailAssetRows(uint64_t mail_id) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return -1;
    std::ostringstream sql;
    // player_item.extra_data 为 JSON 列，用 JSON_EXTRACT 统计该邮件入账行数
    sql << "SELECT COUNT(*) FROM player_item WHERE JSON_EXTRACT(extra_data,'$.mail_id')="
        << mail_id;
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    const int n = row && row[0] ? std::atoi(row[0]) : 0;
    mysql_free_result(res);
    return n;
}

std::string MailAttachmentState(uint64_t mail_id) {
    mail::MailInstanceRow row;
    if (!MailStore::Instance().LoadMail(mail_id, &row))
        return "";
    return row.attachment_state;
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
    PlayerItemStore::Instance().EnsureTable();

    const uint64_t player_id = 900001;
    const uint32_t item_id = 2001;
    const std::string biz = "integ_claim_" + std::to_string(NowMs());

    mail::DeliverRequest d;
    d.source_system = "integ_test";
    d.business_key = biz;
    d.receiver_type = "ROLE";
    d.receiver_id = player_id;
    d.category = "SYSTEM";
    d.title = "concurrent claim test";
    d.body = "integration";
    d.sender_name = "Test";
    mail::DeliverAttachment a;
    a.asset_type = "ITEM";
    a.asset_id = item_id;
    a.count = 3;
    d.attachments.push_back(a);

    uint64_t mail_id = 0;
    std::string ec, msg;
    EXPECT_TRUE(MailService::Instance().Deliver(d, &mail_id, &ec, &msg));
    EXPECT_TRUE(mail_id > 0);
    if (mail_id == 0)
        return 1;

    const uint32_t bag_before = GameLogic::Instance().GetItemCount(player_id, item_id);
    const int db_before = CountMailAssetRows(mail_id);
    EXPECT_TRUE(db_before == 0);

    // ---------- A: 10 线程同一 idempotency_key ----------
    {
        const std::string idem = "claim_same_" + std::to_string(mail_id);
        std::atomic<int> ok_n{0};
        std::atomic<int> already_n{0};
        std::vector<std::thread> ths;
        for (int i = 0; i < 10; ++i) {
            ths.emplace_back([&]() {
                game::MailClaimReq req;
                req.set_player_id(player_id);
                req.set_mail_id(mail_id);
                req.set_idempotency_key(idem);
                req.set_trace_id("integ-a");
                game::GameResponse rsp;
                MailService::Instance().HandleMailClaim(req, &rsp);
                if (rsp.mail_claim().result().error_code() == mail::err::kOk ||
                    rsp.mail_claim().result().error_code() == mail::err::kAlreadyClaimed)
                    ok_n.fetch_add(1);
                if (rsp.mail_claim().result().error_code() == mail::err::kAlreadyClaimed)
                    already_n.fetch_add(1);
            });
        }
        for (auto &t : ths)
            t.join();

        EXPECT_TRUE(MailAttachmentState(mail_id) == "CLAIMED");
        const int db_after = CountMailAssetRows(mail_id);
        EXPECT_TRUE(db_after == 1);
        const uint32_t bag_after = GameLogic::Instance().GetItemCount(player_id, item_id);
        EXPECT_TRUE(bag_after == bag_before + 3);
        EXPECT_TRUE(ok_n.load() >= 1);
        std::printf("case A same-idem: okish=%d already=%d db_rows=%d bag=%u->%u\n", ok_n.load(),
                    already_n.load(), db_after, bag_before, bag_after);
    }

    // ---------- B: 新邮件 + 10 个不同 idempotency_key ----------
    {
        const std::string biz2 = "integ_claim_diff_" + std::to_string(NowMs());
        mail::DeliverRequest d2 = d;
        d2.business_key = biz2;
        uint64_t mail2 = 0;
        EXPECT_TRUE(MailService::Instance().Deliver(d2, &mail2, &ec, &msg));
        EXPECT_TRUE(mail2 > 0);

        const uint32_t bag0 = GameLogic::Instance().GetItemCount(player_id, item_id);
        std::atomic<int> success_ok{0};
        std::atomic<int> already{0};
        std::vector<std::thread> ths;
        for (int i = 0; i < 10; ++i) {
            ths.emplace_back([&, i]() {
                game::MailClaimReq req;
                req.set_player_id(player_id);
                req.set_mail_id(mail2);
                req.set_idempotency_key("claim_diff_" + std::to_string(mail2) + "_" +
                                       std::to_string(i));
                req.set_trace_id("integ-b");
                game::GameResponse rsp;
                MailService::Instance().HandleMailClaim(req, &rsp);
                const std::string &code = rsp.mail_claim().result().error_code();
                if (code == mail::err::kOk)
                    success_ok.fetch_add(1);
                if (code == mail::err::kAlreadyClaimed || code == mail::err::kClaimInProgress)
                    already.fetch_add(1);
            });
        }
        for (auto &t : ths)
            t.join();

        EXPECT_TRUE(MailAttachmentState(mail2) == "CLAIMED");
        EXPECT_TRUE(CountMailAssetRows(mail2) == 1);
        EXPECT_TRUE(success_ok.load() == 1);
        const uint32_t bag1 = GameLogic::Instance().GetItemCount(player_id, item_id);
        EXPECT_TRUE(bag1 == bag0 + 3);
        std::printf("case B diff-idem: ok=%d already/in_progress=%d db_rows=%d bag=%u->%u\n",
                    success_ok.load(), already.load(), CountMailAssetRows(mail2), bag0, bag1);
    }

    // ---------- C: 越权领取 ----------
    {
        const std::string biz3 = "integ_claim_deny_" + std::to_string(NowMs());
        mail::DeliverRequest d3 = d;
        d3.business_key = biz3;
        d3.receiver_id = player_id;
        uint64_t mail3 = 0;
        EXPECT_TRUE(MailService::Instance().Deliver(d3, &mail3, &ec, &msg));
        game::MailClaimReq req;
        req.set_player_id(player_id + 1);
        req.set_mail_id(mail3);
        req.set_idempotency_key("deny_" + std::to_string(mail3));
        game::GameResponse rsp;
        MailService::Instance().HandleMailClaim(req, &rsp);
        EXPECT_TRUE(rsp.mail_claim().result().error_code() == mail::err::kPermissionDenied);
        EXPECT_TRUE(MailAttachmentState(mail3) == "UNCLAIMED");
        EXPECT_TRUE(CountMailAssetRows(mail3) == 0);
        std::printf("case C deny: code=%s\n", rsp.mail_claim().result().error_code().c_str());
    }

    if (g_fail) {
        std::fprintf(stderr, "%d assertion(s) failed\n", g_fail);
        std::fflush(stderr);
        std::_Exit(1);
    }
    std::fprintf(stderr, "mail_claim_integration_test OK\n");
    std::fflush(stderr);
    std::_Exit(0);
}
