#include "AuthServiceImpl.h"

#include "AuthTokenStore.h"
#include "FormalMode.h"
#include "GameMeshPaths.h"
#include "Logging.h"
#include "PasswordHash.h"
#include "PlayerAccountStore.h"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "gamedb.pb.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>

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
        return ch.get();
    }
    ch = std::move(neu);
    cached_addr = addr;
    LOG_INFO << "AuthService GameDB channel ready " << addr;
    return ch.get();
}

struct AuthLookup {
    bool ok = false;
    bool exists = false;
    bool banned = false;
    uint64_t account_id = 0;
    std::string password_hash;
    std::string password_salt;
    int password_iters = 0;
    bool has_password = false;
    std::string err;
};

bool LookupViaGameDb(uint64_t player_id, const std::string &device_id, AuthLookup *out) {
    *out = AuthLookup{};
    brpc::Channel *ch = CachedGameDbChannel();
    if (!ch) {
        out->err = "gamedb channel unavailable";
        return false;
    }
    gdb::GameDbService_Stub stub(ch);
    gdb::LookupAccountReq req;
    req.set_player_id(player_id);
    req.set_device_id(device_id);
    gdb::LookupAccountRsp rsp;
    brpc::Controller cntl;
    stub.LookupAccount(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed() || !rsp.ok()) {
        out->err = cntl.Failed() ? cntl.ErrorText() : rsp.message();
        return false;
    }
    out->ok = true;
    out->exists = rsp.exists();
    out->banned = rsp.banned();
    out->account_id = rsp.account_id() ? rsp.account_id() : player_id;
    out->password_hash = rsp.password_hash();
    out->password_salt = rsp.password_salt();
    out->password_iters = rsp.password_iters();
    out->has_password = rsp.has_password();
    return true;
}

bool LookupLocal(uint64_t player_id, AuthLookup *out) {
    *out = AuthLookup{};
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!PlayerAccountStore::Instance().Available())
        PlayerAccountStore::Instance().EnsureTable();
    AccountAuthRow row;
    if (!PlayerAccountStore::Instance().LoadAuth(player_id, &row)) {
        out->err = "local account load failed";
        return false;
    }
    out->ok = true;
    out->exists = row.exists;
    out->banned = row.banned;
    out->account_id = row.account_id ? row.account_id : player_id;
    out->password_hash = row.password_hash;
    out->password_salt = row.password_salt;
    out->password_iters = row.password_iters;
    out->has_password = row.has_password;
    return true;
#else
    (void)player_id;
    out->err = "mysql not enabled";
    return false;
#endif
}

bool LookupAccount(uint64_t player_id, const std::string &device_id, AuthLookup *out) {
    if (LookupViaGameDb(player_id, device_id, out))
        return true;
    if (FormalModeEnabled()) {
        out->err = out->err.empty() ? "formal mode: gamedb required" : out->err;
        return false;
    }
    LOG_WARN << "AuthService GameDB lookup failed; DEV fallback to local PlayerAccountStore"
             << " err=" << out->err;
    return LookupLocal(player_id, out);
}

/** 简易登录失败限流：账号维度 */
bool LoginRateLimited(uint64_t player_id) {
    static std::mutex mu;
    static std::unordered_map<uint64_t, std::pair<int, int64_t>> fails;
    using namespace std::chrono;
    const int64_t now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(mu);
    auto &e = fails[player_id];
    if (now - e.second > 60) {
        e = {0, now};
    }
    if (e.first >= 10)
        return true;
    return false;
}

void RecordLoginFail(uint64_t player_id) {
    static std::mutex mu;
    static std::unordered_map<uint64_t, std::pair<int, int64_t>> fails;
    using namespace std::chrono;
    const int64_t now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(mu);
    auto &e = fails[player_id];
    if (now - e.second > 60)
        e = {0, now};
    e.first += 1;
    e.second = now;
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
    if (LoginRateLimited(request->player_id())) {
        response->set_ok(false);
        response->set_error_code("RATE_LIMITED");
        response->set_message("too many login failures");
        return;
    }

    if (!AuthTokenStore::Instance().Available())
        AuthTokenStore::Instance().InitFromConfig();

    AuthLookup lu;
    if (!LookupAccount(request->player_id(), request->device_id(), &lu)) {
        response->set_ok(false);
        response->set_error_code("ACCOUNT_LOOKUP_FAILED");
        response->set_message(lu.err.empty() ? "account lookup failed" : lu.err);
        return;
    }
    if (!lu.exists) {
        response->set_ok(false);
        response->set_error_code("ACCOUNT_NOT_FOUND");
        response->set_message("account not registered");
        RecordLoginFail(request->player_id());
        return;
    }
    if (lu.banned) {
        response->set_ok(false);
        response->set_error_code("BANNED");
        response->set_message("account banned");
        response->set_banned(true);
        return;
    }

    // 有密码的账号必须校验；无密码旧账号仅在非正式模式允许空凭证
    if (lu.has_password) {
        if (request->credential().empty() ||
            !PasswordHash::VerifyPassword(request->credential(), lu.password_salt, lu.password_iters,
                                          lu.password_hash)) {
            response->set_ok(false);
            response->set_error_code("BAD_CREDENTIAL");
            response->set_message("invalid credential");
            RecordLoginFail(request->player_id());
            LOG_INFO << "AuthService Login fail player_id=" << request->player_id()
                     << " reason=bad_credential";
            return;
        }
    } else if (FormalModeEnabled()) {
        response->set_ok(false);
        response->set_error_code("PASSWORD_REQUIRED");
        response->set_message("account has no password; re-register in formal mode");
        return;
    }

    std::string access;
    if (AuthTokenStore::Instance().Available()) {
        if (!AuthTokenStore::Instance().IssueAccessToken(request->player_id(), lu.account_id, 3600,
                                                         &access)) {
            LOG_WARN << "AuthTokenStore IssueAccessToken failed; continue without access_token";
        }
    }

    response->set_ok(true);
    response->set_message("auth ok");
    response->set_account_id(lu.account_id);
    response->set_player_id(request->player_id());
    response->set_banned(false);
    if (!access.empty()) {
        response->set_access_token(access);
        response->set_refresh_token(access);
    }
    LOG_INFO << "AuthService Login ok player_id=" << request->player_id()
             << " access=" << PasswordHash::RedactSecret(access);
}

void AuthServiceImpl::Register(::google::protobuf::RpcController *controller,
                               const ::auth::RegisterRequest *request,
                               ::auth::RegisterResponse *response,
                               ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->device_id().empty() || request->password().size() < 6) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("device_id and password(>=6) required");
        return;
    }

    std::string salt;
    std::string hash;
    if (!PasswordHash::GenerateSalt(&salt) ||
        !PasswordHash::HashPassword(request->password(), salt, PasswordHash::kDefaultIterations,
                                    &hash)) {
        response->set_ok(false);
        response->set_error_code("HASH_FAILED");
        response->set_message("password hash failed");
        return;
    }

    brpc::Channel *ch = CachedGameDbChannel();
    if (!ch) {
        if (FormalModeEnabled()) {
            response->set_ok(false);
            response->set_error_code("GAMEDB_REQUIRED");
            response->set_message("formal mode: register requires GameDB");
            return;
        }
#ifdef WEBSERVER_ENABLE_MYSQL
        uint64_t pid = 0;
        std::string err;
        if (!PlayerAccountStore::Instance().RegisterWithPassword(
                request->device_id(), request->display_name(), hash, salt,
                PasswordHash::kDefaultIterations, &pid, &err)) {
            response->set_ok(false);
            response->set_error_code("REGISTER_FAILED");
            response->set_message(err);
            return;
        }
        response->set_ok(true);
        response->set_message("ok");
        response->set_player_id(pid);
        response->set_account_id(pid);
        LOG_WARN << "AuthService Register DEV local MySQL player_id=" << pid;
        return;
#else
        response->set_ok(false);
        response->set_error_code("MYSQL_DISABLED");
        response->set_message("mysql not enabled");
        return;
#endif
    }

    gdb::GameDbService_Stub stub(ch);
    gdb::RegisterAccountReq req;
    req.set_device_id(request->device_id());
    req.set_display_name(request->display_name());
    req.set_password_hash(hash);
    req.set_password_salt(salt);
    req.set_password_iters(PasswordHash::kDefaultIterations);
    req.set_idempotency_key(request->device_id() + ":" + salt.substr(0, 8));
    gdb::RegisterAccountRsp rsp;
    brpc::Controller cntl;
    stub.RegisterAccount(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed() || !rsp.ok()) {
        response->set_ok(false);
        response->set_error_code(rsp.error_code().empty() ? "REGISTER_FAILED" : rsp.error_code());
        response->set_message(cntl.Failed() ? cntl.ErrorText() : rsp.message());
        return;
    }
    response->set_ok(true);
    response->set_message("ok");
    response->set_player_id(rsp.player_id());
    response->set_account_id(rsp.account_id());
    LOG_INFO << "AuthService Register ok player_id=" << rsp.player_id()
             << " device=" << request->device_id();
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
