/**
 * @file mail_brpc_client.cpp
 * @brief brpc 客户端：Login → MailList（及可选 summary/get/claim 等）
 *
 * 用法：
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --summary
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --get=MAIL_ID
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --claim=MAIL_ID
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --read=MAIL_ID
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --favorite=MAIL_ID
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --batch_read=1,2,3
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --batch_delete=1,2
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --batch_claim=1,2
 *   ./mail_brpc_client --server=127.0.0.1:8181 --player_id=10001 --deliver
 */

#include "mail_brpc.pb.h"

#include <brpc/channel.h>
#include <gflags/gflags.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

DEFINE_string(server, "127.0.0.1:8181", "brpc server ip:port");
DEFINE_uint64(player_id, 10001, "player id");
DEFINE_string(device_id, "mail_brpc_client", "login device id");
DEFINE_uint32(server_id, 1, "login server id");
DEFINE_int32(limit, 20, "mail list limit");
DEFINE_bool(summary, false, "also call MailboxSummary");
DEFINE_uint64(get, 0, "MailGet mail_id (0=skip)");
DEFINE_uint64(claim, 0, "MailClaim mail_id (0=skip)");
DEFINE_uint64(read, 0, "MailRead mail_id (0=skip)");
DEFINE_uint64(favorite, 0, "MailFavorite mail_id (0=skip)");
DEFINE_string(batch_read, "", "comma-separated mail ids for MailBatchRead");
DEFINE_string(batch_delete, "", "comma-separated mail ids for MailBatchDelete");
DEFINE_string(batch_claim, "", "comma-separated mail ids for MailBatchClaim");
DEFINE_bool(deliver, false, "demo MailDeliver one system mail to player_id");

namespace {

std::vector<uint64_t> ParseIds(const std::string &s) {
    std::vector<uint64_t> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty())
            continue;
        out.push_back(static_cast<uint64_t>(std::strtoull(item.c_str(), nullptr, 10)));
    }
    return out;
}

bool Check(brpc::Controller &cntl, const char *op) {
    if (cntl.Failed()) {
        std::fprintf(stderr, "FAIL %s: %s\n", op, cntl.ErrorText().c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 3000;
    options.max_retry = 1;
    if (channel.Init(FLAGS_server.c_str(), &options) != 0) {
        std::fprintf(stderr, "FAIL channel.Init %s\n", FLAGS_server.c_str());
        return 1;
    }

    mailrpc::MailBrpcService_Stub stub(&channel);

    // --- Login ---
    game::LoginReq login_req;
    login_req.set_player_id(FLAGS_player_id);
    login_req.set_device_id(FLAGS_device_id);
    login_req.set_server_id(FLAGS_server_id);
    game::LoginRsp login_rsp;
    {
        brpc::Controller cntl;
        stub.Login(&cntl, &login_req, &login_rsp, nullptr);
        if (!Check(cntl, "Login") || !login_rsp.ok()) {
            std::fprintf(stderr, "FAIL Login: %s\n", login_rsp.message().c_str());
            return 1;
        }
    }
    const std::string token = login_rsp.token();
    std::printf("OK Login token=%s server_id=%u\n", token.c_str(), login_rsp.server_id());

    // --- MailList（主路径）---
    mailrpc::AuthedMailListReq list_req;
    list_req.set_session_token(token);
    list_req.mutable_req()->set_player_id(FLAGS_player_id);
    list_req.mutable_req()->set_limit(FLAGS_limit);
    game::MailListRsp list_rsp;
    {
        brpc::Controller cntl;
        stub.MailList(&cntl, &list_req, &list_rsp, nullptr);
        if (!Check(cntl, "MailList") || !list_rsp.ok()) {
            std::fprintf(stderr, "FAIL MailList: %s code=%s\n", list_rsp.message().c_str(),
                         list_rsp.error_code().c_str());
            return 1;
        }
    }
    std::printf("OK MailList count=%d next_cursor=%s mailbox_version=%lld\n",
                list_rsp.mails_size(), list_rsp.next_cursor().c_str(),
                static_cast<long long>(list_rsp.mailbox_version()));
    const int show = list_rsp.mails_size() < 5 ? list_rsp.mails_size() : 5;
    for (int i = 0; i < show; ++i) {
        const auto &m = list_rsp.mails(i);
        std::printf("  [%d] id=%llu title=%s read=%s attach=%d\n", i,
                    static_cast<unsigned long long>(m.mail_id()), m.title().c_str(),
                    m.read_state().c_str(), m.has_attachment() ? 1 : 0);
    }

    if (FLAGS_summary) {
        mailrpc::AuthedMailboxSummaryReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        game::MailboxSummaryRsp rsp;
        brpc::Controller cntl;
        stub.MailboxSummary(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailboxSummary") && rsp.ok())
            std::printf("OK MailboxSummary unread_system=%u current=%u/%u\n", rsp.unread_system(),
                        rsp.current_count(), rsp.max_capacity());
    }

    if (FLAGS_get != 0) {
        mailrpc::AuthedMailGetReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        req.mutable_req()->set_mail_id(FLAGS_get);
        game::MailGetRsp rsp;
        brpc::Controller cntl;
        stub.MailGet(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailGet") && rsp.ok())
            std::printf("OK MailGet id=%llu body_len=%zu\n",
                        static_cast<unsigned long long>(rsp.mail().brief().mail_id()),
                        rsp.mail().body().size());
    }

    if (FLAGS_read != 0) {
        mailrpc::AuthedMailReadReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        req.mutable_req()->set_mail_id(FLAGS_read);
        req.mutable_req()->set_idempotency_key("client-read-" + std::to_string(FLAGS_read));
        game::MailReadRsp rsp;
        brpc::Controller cntl;
        stub.MailRead(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailRead") && rsp.ok())
            std::printf("OK MailRead read_at=%lld\n", static_cast<long long>(rsp.read_at()));
    }

    if (FLAGS_claim != 0) {
        mailrpc::AuthedMailClaimReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        req.mutable_req()->set_mail_id(FLAGS_claim);
        req.mutable_req()->set_idempotency_key("client-claim-" + std::to_string(FLAGS_claim));
        game::MailClaimRsp rsp;
        brpc::Controller cntl;
        stub.MailClaim(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailClaim") && rsp.ok())
            std::printf("OK MailClaim mail_id=%llu attach_state=%s\n",
                        static_cast<unsigned long long>(rsp.result().mail_id()),
                        rsp.result().attachment_state().c_str());
    }

    if (FLAGS_favorite != 0) {
        mailrpc::AuthedMailFavoriteReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        req.mutable_req()->set_mail_id(FLAGS_favorite);
        req.mutable_req()->set_favorite(true);
        game::MailFavoriteRsp rsp;
        brpc::Controller cntl;
        stub.MailFavorite(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailFavorite") && rsp.ok())
            std::printf("OK MailFavorite is_favorite=%d\n", rsp.is_favorite() ? 1 : 0);
    }

    if (!FLAGS_batch_read.empty()) {
        mailrpc::AuthedMailBatchReadReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        for (uint64_t id : ParseIds(FLAGS_batch_read))
            req.mutable_req()->add_mail_ids(id);
        game::MailBatchReadRsp rsp;
        brpc::Controller cntl;
        stub.MailBatchRead(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailBatchRead") && rsp.ok())
            std::printf("OK MailBatchRead success_count=%u\n", rsp.success_count());
    }

    if (!FLAGS_batch_delete.empty()) {
        mailrpc::AuthedMailBatchDeleteReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        for (uint64_t id : ParseIds(FLAGS_batch_delete))
            req.mutable_req()->add_mail_ids(id);
        game::MailBatchDeleteRsp rsp;
        brpc::Controller cntl;
        stub.MailBatchDelete(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailBatchDelete") && rsp.ok())
            std::printf("OK MailBatchDelete success_count=%u\n", rsp.success_count());
    }

    if (!FLAGS_batch_claim.empty()) {
        mailrpc::AuthedMailBatchClaimReq req;
        req.set_session_token(token);
        req.mutable_req()->set_player_id(FLAGS_player_id);
        for (uint64_t id : ParseIds(FLAGS_batch_claim))
            req.mutable_req()->add_mail_ids(id);
        req.mutable_req()->set_idempotency_key_prefix("client-bclaim-");
        game::MailBatchClaimRsp rsp;
        brpc::Controller cntl;
        stub.MailBatchClaim(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailBatchClaim") && rsp.ok())
            std::printf("OK MailBatchClaim results=%d\n", rsp.results_size());
    }

    if (FLAGS_deliver) {
        mailrpc::AuthedMailDeliverReq req;
        req.set_session_token(token);
        auto *body = req.mutable_req();
        body->set_source_system("mail_brpc_client");
        body->set_business_key("demo-" + std::to_string(FLAGS_player_id) + "-" +
                               std::to_string(time(nullptr)));
        body->set_receiver_type("ROLE");
        body->set_receiver_id(FLAGS_player_id);
        body->set_category("SYSTEM");
        body->set_title("brpc demo mail");
        body->set_body("delivered via MailBrpcService");
        body->set_sender_name("system");
        game::MailDeliverRsp rsp;
        brpc::Controller cntl;
        stub.MailDeliver(&cntl, &req, &rsp, nullptr);
        if (Check(cntl, "MailDeliver") && rsp.ok())
            std::printf("OK MailDeliver mail_id=%llu\n",
                        static_cast<unsigned long long>(rsp.mail_id()));
    }

    std::printf("PASS mail_brpc_client\n");
    return 0;
}
