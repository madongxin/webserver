/**
 * 玩家邮件：合法发送、幂等、拒绝自报伪造由 TrustedPlayerId 覆盖；MailDeliver 仍危险。
 */
#include "AsyncMysqlGameDbRepository.h"
#include "ConnectionPool.h"
#include "Logging.h"
#include "MailService.h"
#include "PlayerAccountStore.h"
#include "PlayerProfileStore.h"
#include "game.pb.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t RegisterOne(const std::string &tag) {
    uint64_t pid = 0;
    std::string err;
    const std::string device = "s1_mail_" + tag;
    const std::string idem = "s1_mail_idem_" + tag;
    if (!PlayerAccountStore::Instance().RegisterWithPasswordIdempotent(
            device, tag, "hashhashhashhash", "saltsalt", 1000, idem, &pid, &err, nullptr))
        return 0;
    return pid;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        std::printf("FAIL: MySQL pool not initialized\n");
        return 1;
    }
    if (!MailService::Instance().Init()) {
        std::printf("FAIL: MailService::Init\n");
        return 1;
    }
    const std::string tag = std::to_string(NowMs());
    const uint64_t a = RegisterOne("A" + tag);
    const uint64_t b = RegisterOne("B" + tag);
    Expect(a != 0 && b != 0 && a != b, "two accounts");

    game::PlayerMailSendReq send;
    send.set_sender_player_id(a);
    send.set_receiver_player_id(b);
    send.set_title("hello");
    send.set_body("mail body from A");
    send.set_operation_id("op_" + tag);
    game::GameResponse rsp;
    Expect(MailService::Instance().HandlePlayerMailSend(send, &rsp), "send ok");
    Expect(rsp.player_mail_send().ok() && rsp.player_mail_send().mail_id() != 0, "mail id");
    const uint64_t mid = rsp.player_mail_send().mail_id();

    game::GameResponse rsp2;
    Expect(MailService::Instance().HandlePlayerMailSend(send, &rsp2), "retry");
    Expect(rsp2.player_mail_send().ok() && rsp2.player_mail_send().idempotent_hit(), "idempotent");
    Expect(rsp2.player_mail_send().mail_id() == mid, "same mail");

    game::MailListReq list;
    list.set_player_id(b);
    list.set_limit(20);
    game::GameResponse listed;
    Expect(MailService::Instance().HandleMailList(list, &listed), "list");
    bool found = false;
    for (int i = 0; i < listed.mail_list().mails_size(); ++i) {
        if (listed.mail_list().mails(i).mail_id() == mid)
            found = true;
    }
    Expect(found, "receiver sees mail");

    game::MailReadReq read;
    read.set_player_id(b);
    read.set_mail_id(mid);
    read.set_idempotency_key("read_" + tag);
    game::GameResponse read_rsp;
    Expect(MailService::Instance().HandleMailRead(read, &read_rsp), "read");
    Expect(read_rsp.mail_read().ok(), "read ok");
    read.set_mail_id(mid + 1);
    game::GameResponse read_other;
    Expect(!MailService::Instance().HandleMailRead(read, &read_other), "read key other mail");
    Expect(read_other.mail_read().error_code() == "IDEMPOTENCY_CONFLICT", "read conflict");

    mail::DeliverRequest d;
    d.source_system = "activity";
    d.business_key = "biz_" + tag;
    d.receiver_id = b;
    d.category = "ACTIVITY";
    d.title = "reward";
    d.body = "one item";
    mail::DeliverAttachment att;
    att.asset_type = "ITEM";
    att.asset_id = 11;
    att.count = 1;
    d.attachments.push_back(att);
    uint64_t delivered = 0;
    std::string ec, msg;
    Expect(MailService::Instance().Deliver(d, &delivered, &ec, &msg), "deliver");
    att.count = 2;
    d.attachments[0] = att;
    uint64_t delivered2 = 0;
    Expect(!MailService::Instance().Deliver(d, &delivered2, &ec, &msg), "deliver conflict");
    Expect(ec == "IDEMPOTENCY_CONFLICT", "attachment conflict");
    Expect(delivered2 == 0, "no second mail id");

    game::MailGetReq get;
    get.set_player_id(b);
    get.set_mail_id(delivered);
    game::GameResponse get_rsp;
    Expect(MailService::Instance().HandleMailGet(get, &get_rsp), "get attachment mail");
    Expect(get_rsp.mail_get().ok() && get_rsp.mail_get().mail().attachments_size() == 1,
           "attachment visible");
    get.set_mail_id(9000000000001ull);
    game::GameResponse missing_get;
    Expect(!MailService::Instance().HandleMailGet(get, &missing_get), "missing mail");
    Expect(missing_get.mail_get().error_code() == "MAIL_NOT_FOUND", "not found");

    game::MailFavoriteReq fav;
    fav.set_player_id(b);
    fav.set_mail_id(mid);
    fav.set_favorite(true);
    fav.set_idempotency_key("fav_" + tag);
    game::GameResponse fav_rsp;
    Expect(MailService::Instance().HandleMailFavorite(fav, &fav_rsp), "favorite");
    get.set_mail_id(mid);
    game::GameResponse fav_get;
    Expect(MailService::Instance().HandleMailGet(get, &fav_get), "get favorite");
    bool unfav = false;
    for (int i = 0; i < fav_get.mail_get().mail().allowed_actions_size(); ++i) {
        if (fav_get.mail_get().mail().allowed_actions(i) == "UNFAVORITE")
            unfav = true;
    }
    Expect(unfav, "unfavorite action");
    fav.set_favorite(false);
    fav.set_idempotency_key("unfav_" + tag);
    Expect(MailService::Instance().HandleMailFavorite(fav, &fav_rsp), "unfavorite");

    game::MailBatchDeleteReq keep;
    keep.set_player_id(b);
    keep.add_mail_ids(delivered);
    keep.set_idempotency_key("del_att_" + tag);
    game::GameResponse keep_rsp;
    Expect(MailService::Instance().HandleMailBatchDelete(keep, &keep_rsp), "delete unclaimed call");
    Expect(keep_rsp.mail_batch_delete().success_count() == 0, "unclaimed not deleted");
    Expect(keep_rsp.mail_batch_delete().failed_mail_ids_size() == 1, "unclaimed failed id");
    get.set_mail_id(delivered);
    Expect(MailService::Instance().HandleMailGet(get, &get_rsp), "unclaimed still readable");
    Expect(get_rsp.mail_get().mail().brief().visible_state() == "ACTIVE", "still active");

    game::MailBatchDeleteReq drop;
    drop.set_player_id(b);
    drop.add_mail_ids(mid);
    drop.set_idempotency_key("del_plain_" + tag);
    game::GameResponse drop_rsp;
    Expect(MailService::Instance().HandleMailBatchDelete(drop, &drop_rsp), "delete plain");
    Expect(drop_rsp.mail_batch_delete().success_count() == 1, "deleted one");
    game::GameResponse drop_retry;
    Expect(MailService::Instance().HandleMailBatchDelete(drop, &drop_retry), "delete replay");
    Expect(drop_retry.mail_batch_delete().success_count() == 1, "replay count");

    game::PlayerMailSendReq bad;
    bad.set_sender_player_id(a);
    bad.set_receiver_player_id(a);
    bad.set_title("x");
    bad.set_body("y");
    bad.set_operation_id("self_" + tag);
    game::GameResponse self_rsp;
    Expect(!MailService::Instance().HandlePlayerMailSend(bad, &self_rsp), "no self mail");

    game::PlayerMailSendReq missing;
    missing.set_sender_player_id(a);
    missing.set_receiver_player_id(9000000000001ull);
    missing.set_title("x");
    missing.set_body("y");
    missing.set_operation_id("miss_" + tag);
    game::GameResponse miss_rsp;
    Expect(!MailService::Instance().HandlePlayerMailSend(missing, &miss_rsp), "missing receiver");
    Expect(miss_rsp.player_mail_send().error_code() == "ERR_RECEIVER_NOT_FOUND" ||
               miss_rsp.player_mail_send().error_code() == "ERR_MAIL_SELF" ||
               !miss_rsp.player_mail_send().ok(),
           "receiver error");

    if (fails) {
        std::printf("player_mail_send_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK player_mail_send_test a=%llu b=%llu mail=%llu\n",
                static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                static_cast<unsigned long long>(mid));
    return 0;
}
