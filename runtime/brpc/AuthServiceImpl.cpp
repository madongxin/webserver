#include "AuthServiceImpl.h"

#include "AuthTokenStore.h"
#include "GameMeshPaths.h"
#include "Logging.h"
#include "PlayerAccountStore.h"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "gamedb.pb.h"

#include <fstream>
#include <memory>
#include <mutex>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

std::string ReadCnfValue(const std::string &path, const char *key) {
    std::ifstream in(path);
    if (!in || !key)
        return {};
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (Trim(line.substr(0, eq)) == key)
            return Trim(line.substr(eq + 1));
    }
    return {};
}

std::string ResolveGamedbAddr() {
    std::string session_cnf = "../config/session.cnf";
    std::string resolved;
    if (GameMeshPaths::ResolveProjectSubdir("config/session.cnf", &resolved))
        session_cnf = resolved;
    std::string gamedb = ReadCnfValue(session_cnf, "gamedb_addrs");
    if (gamedb.empty()) {
        std::string gw = "../config/gateway.cnf";
        if (GameMeshPaths::ResolveProjectSubdir("config/gateway.cnf", &resolved))
            gw = resolved;
        gamedb = ReadCnfValue(gw, "gamedb_addrs");
    }
    if (gamedb.empty())
        return {};
    const auto comma = gamedb.find(',');
    if (comma != std::string::npos)
        gamedb = gamedb.substr(0, comma);
    return Trim(gamedb);
}

/** 运行期复用 Channel，禁止逐请求 Init */
brpc::Channel *CachedGameDbChannel() {
    static std::mutex mu;
    static std::unique_ptr<brpc::Channel> ch;
    static std::string cached_addr;
    const std::string addr = ResolveGamedbAddr();
    if (addr.empty())
        return nullptr;
    std::lock_guard<std::mutex> lk(mu);
    if (ch && cached_addr == addr)
        return ch.get();
    auto neu = std::unique_ptr<brpc::Channel>(new brpc::Channel());
    brpc::ChannelOptions opt;
    opt.timeout_ms = 2000;
    opt.max_retry = 0;
    if (neu->Init(addr.c_str(), &opt) != 0) {
        LOG_WARN << "AuthService GameDB channel init failed addr=" << addr;
        return ch.get();  // 保留旧 Channel（若有）
    }
    ch = std::move(neu);
    cached_addr = addr;
    LOG_INFO << "AuthService GameDB channel ready " << addr;
    return ch.get();
}

bool LookupAccountViaGameDbOrLocal(uint64_t player_id, const std::string &device_id, bool *exists,
                                   bool *banned, uint64_t *account_id, std::string *err) {
    *exists = false;
    *banned = false;
    *account_id = player_id;
#ifdef WEBSERVER_ENABLE_MYSQL
    if (brpc::Channel *ch = CachedGameDbChannel()) {
        gdb::GameDbService_Stub stub(ch);
        gdb::LookupAccountReq req;
        req.set_player_id(player_id);
        req.set_device_id(device_id);
        gdb::LookupAccountRsp rsp;
        brpc::Controller cntl;
        stub.LookupAccount(&cntl, &req, &rsp, nullptr);
        if (!cntl.Failed() && rsp.ok()) {
            *exists = rsp.exists();
            *banned = rsp.banned();
            *account_id = rsp.account_id() ? rsp.account_id() : player_id;
            if (!*exists && err)
                *err = rsp.message().empty() ? "account not found" : rsp.message();
            return true;
        }
    }
    // MVP 降级：Auth 进程内 PlayerAccountStore → 同一 MySQL（与 GameDB 同源）
    if (!PlayerAccountStore::Instance().Available())
        PlayerAccountStore::Instance().EnsureTable();
    *exists = PlayerAccountStore::Instance().Exists(player_id);
    if (!*exists && err)
        *err = "account not registered";
    return true;
#else
    (void)device_id;
    if (err)
        *err = "mysql not enabled";
    return false;
#endif
}

}  // namespace

void AuthServiceImpl::Login(::google::protobuf::RpcController *controller,
                            const ::auth::LoginRequest *request, ::auth::LoginResponse *response,
                            ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0 || request->device_id().empty()) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("player_id and device_id required");
        return;
    }
    // 凭证仅在 Auth 消费，绝不创建 Session、不转发 GameLogic
    (void)request->credential();

    if (!AuthTokenStore::Instance().Available())
        AuthTokenStore::Instance().InitFromConfig();

    bool exists = false;
    bool banned = false;
    uint64_t account_id = request->player_id();
    std::string err;
    if (!LookupAccountViaGameDbOrLocal(request->player_id(), request->device_id(), &exists, &banned,
                                       &account_id, &err)) {
        response->set_ok(false);
        response->set_error_code("ACCOUNT_LOOKUP_FAILED");
        response->set_message(err.empty() ? "account lookup failed" : err);
        return;
    }
    if (!exists) {
        response->set_ok(false);
        response->set_error_code("ACCOUNT_NOT_FOUND");
        response->set_message(err.empty() ? "account not registered" : err);
        return;
    }
    if (banned) {
        response->set_ok(false);
        response->set_error_code("BANNED");
        response->set_message("account banned");
        response->set_banned(true);
        return;
    }

    std::string access;
    if (AuthTokenStore::Instance().Available()) {
        if (!AuthTokenStore::Instance().IssueAccessToken(request->player_id(), account_id, 3600,
                                                         &access)) {
            LOG_WARN << "AuthTokenStore IssueAccessToken failed; continue without access_token";
        }
    }

    response->set_ok(true);
    response->set_message("auth ok");
    response->set_account_id(account_id);
    response->set_player_id(request->player_id());
    response->set_banned(false);
    if (!access.empty()) {
        response->set_access_token(access);
        response->set_refresh_token(access);
    }
    LOG_INFO << "AuthService Login ok player_id=" << request->player_id()
             << " (no Session created here)";
}

void AuthServiceImpl::VerifyToken(::google::protobuf::RpcController *controller,
                                  const ::auth::VerifyTokenRequest *request,
                                  ::auth::VerifyTokenResponse *response,
                                  ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!AuthTokenStore::Instance().Available())
        AuthTokenStore::Instance().InitFromConfig();
    std::string err;
    const bool ok = AuthTokenStore::Instance().VerifyAccessToken(
        request->player_id(), request->access_token(), &err);
    response->set_ok(ok);
    response->set_message(ok ? "ok" : err);
    if (ok) {
        response->set_account_id(request->player_id());
        response->set_player_id(request->player_id());
    }
}

void AuthServiceImpl::RefreshToken(::google::protobuf::RpcController *controller,
                                   const ::auth::RefreshTokenRequest *request,
                                   ::auth::RefreshTokenResponse *response,
                                   ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!AuthTokenStore::Instance().Available())
        AuthTokenStore::Instance().InitFromConfig();
    std::string err;
    std::string neu;
    if (!AuthTokenStore::Instance().RefreshAccessToken(request->player_id(), request->refresh_token(),
                                                       3600, &neu, &err)) {
        response->set_ok(false);
        response->set_message(err.empty() ? "refresh failed" : err);
        return;
    }
    response->set_ok(true);
    response->set_access_token(neu);
    response->set_refresh_token(neu);
}
