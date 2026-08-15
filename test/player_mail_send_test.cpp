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
