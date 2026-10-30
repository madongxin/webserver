#include "AuthServiceImpl.h"

#include "AuthTokenStore.h"
#include "AuthLoginRateLimit.h"
#include "BrpcNamingUtil.h"
#include "FormalMode.h"
#include "GameMeshPaths.h"
#include "Logging.h"
#include "PasswordExec.h"
#include "PasswordHash.h"
#include "PlayerAccountStore.h"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include "gamedb.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

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

std::vector<std::string> SplitAddrs(const std::string &gamedb) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < gamedb.size()) {
        const auto comma = gamedb.find(',', i);
        const auto part = Trim(gamedb.substr(i, comma == std::string::npos ? std::string::npos : comma - i));
        if (!part.empty())
            out.push_back(part);
        if (comma == std::string::npos)
            break;
        i = comma + 1;
    }
    return out;
}

std::string ReadGamedbList() {
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
    return gamedb;
}

brpc::Channel *CachedGameDbChannel() {
    static std::mutex mu;
    static std::unique_ptr<brpc::Channel> ch;
    static std::string cached_naming;
    const auto addrs = SplitAddrs(ReadGamedbList());
    std::lock_guard<std::mutex> lk(mu);
    // 配置路径暂时解析失败时保留已经连上的 channel，避免健康检查把 GameDB 判死。
    if (addrs.empty())
        return ch.get();
    const std::string naming = BuildListNamingUrl(addrs);
    const std::string lb = SessionLoadBalancerName(addrs.size());
    if (ch && cached_naming == naming)
        return ch.get();
    auto neu = std::unique_ptr<brpc::Channel>(new brpc::Channel());
    brpc::ChannelOptions opt;
    opt.timeout_ms = 2000;
    opt.max_retry = 0;
    const int rc = lb.empty() ? neu->Init(naming.c_str(), &opt)
                              : neu->Init(naming.c_str(), lb.c_str(), &opt);
    if (rc != 0) {
        LOG_WARN << "AuthService GameDB channel init failed naming=" << naming;
        return ch.get();
    }
    ch = std::move(neu);
    cached_naming = naming;
    LOG_INFO << "AuthService GameDB channel ready " << naming
             << " lb=" << (lb.empty() ? "none" : lb);
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

/** 登录失败限流：Redis 共享状态（多 Auth 实例）；不可用时进程内降级 */
bool LoginRateLimited(uint64_t player_id) {
    return AuthLoginRateLimit::Instance().IsLimited(player_id);
}

void RecordLoginFail(uint64_t player_id) {
    AuthLoginRateLimit::Instance().RecordFail(player_id);
}

void ClearLoginFail(uint64_t player_id) {
    AuthLoginRateLimit::Instance().Clear(player_id);
}

// PBKDF2 一次大约几百毫秒。放在 brpc 工作线程上会把同进程的进图、改路由全部排在后面。
void FinishLoginOk(uint64_t player_id, uint64_t account_id, ::auth::LoginResponse *response) {
    std::string access;
    std::string refresh;
    if (AuthTokenStore::Instance().Available()) {
        if (!AuthTokenStore::Instance().IssueTokenPair(player_id, account_id, 3600, 86400 * 7,
                                                       &access, &refresh)) {
            LOG_WARN << "AuthTokenStore IssueTokenPair failed; continue without tokens";
        }
    }
    ClearLoginFail(player_id);
    response->set_ok(true);
    response->set_message("auth ok");
    response->set_account_id(account_id);
    response->set_player_id(player_id);
    response->set_banned(false);
    if (!access.empty()) {
        response->set_access_token(access);
        response->set_refresh_token(refresh);
    }
    static std::atomic<uint32_t> login_logs{0};
    if ((login_logs.fetch_add(1, std::memory_order_relaxed) & 31) == 0) {
        LOG_INFO << "AuthService Login ok player_id=" << player_id
                 << " access=" << PasswordHash::RedactSecret(access)
                 << " refresh=" << PasswordHash::RedactSecret(refresh);
    }
}

}  // namespace

bool AuthGameDbReachable(int timeout_ms) {
    brpc::Channel *ch = CachedGameDbChannel();
    if (!ch)
        return false;
    gdb::LoadPlayerReq req;
    gdb::LoadPlayerRsp rsp;
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms > 0 ? timeout_ms : 800);
    req.set_player_id(0);
    gdb::GameDbService_Stub stub(ch);
    stub.LoadPlayer(&cntl, &req, &rsp, nullptr);
    return !cntl.Failed();
}

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

    // 有密码的账号必须校验；无密码旧账号仅在非正式模式允许空凭证。
    // 校验放到独立线程，归还 brpc 工作线程，进图和改路由不再排在密码哈希后面。
    if (lu.has_password) {
        if (request->credential().empty()) {
            response->set_ok(false);
            response->set_error_code("BAD_CREDENTIAL");
            response->set_message("invalid credential");
            RecordLoginFail(request->player_id());
            LOG_INFO << "AuthService Login fail player_id=" << request->player_id()
                     << " reason=bad_credential";
            return;
        }
        const uint64_t player_id = request->player_id();
        const uint64_t account_id = lu.account_id;
        const std::string credential = request->credential();
        const std::string salt = lu.password_salt;
        const std::string hash = lu.password_hash;
        const int iters = lu.password_iters;
        done_guard.release();
        auto *rsp = response;
        auto finish = [player_id, account_id, credential, salt, hash, iters, rsp, done]() {
            if (!PasswordHash::VerifyPassword(credential, salt, iters, hash)) {
                rsp->set_ok(false);
                rsp->set_error_code("BAD_CREDENTIAL");
                rsp->set_message("invalid credential");
                RecordLoginFail(player_id);
                LOG_INFO << "AuthService Login fail player_id=" << player_id
                         << " reason=bad_credential";
            } else {
                FinishLoginOk(player_id, account_id, rsp);
            }
            if (done)
                done->Run();
        };
        if (!PasswordExec::Instance().Post(finish))
            finish();
        return;
    }
    if (FormalModeEnabled()) {
        response->set_ok(false);
        response->set_error_code("PASSWORD_REQUIRED");
        response->set_message("account has no password; re-register in formal mode");
        return;
    }
    FinishLoginOk(request->player_id(), lu.account_id, response);
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

    const std::string device_id = request->device_id();
    const std::string display_name = request->display_name();
    const std::string password = request->password();
    std::string idem = request->idempotency_key();
    done_guard.release();
    auto *rsp = response;
    auto finish = [device_id, display_name, password, idem, rsp, done]() mutable {
        std::string salt;
        std::string hash;
        if (!PasswordHash::GenerateSalt(&salt) ||
            !PasswordHash::HashPassword(password, salt, PasswordHash::kDefaultIterations, &hash)) {
            rsp->set_ok(false);
            rsp->set_error_code("HASH_FAILED");
            rsp->set_message("password hash failed");
            if (done)
                done->Run();
            return;
        }
        brpc::Channel *ch = CachedGameDbChannel();
        if (!ch) {
            if (FormalModeEnabled()) {
                rsp->set_ok(false);
                rsp->set_error_code("GAMEDB_REQUIRED");
                rsp->set_message("formal mode: register requires GameDB");
                if (done)
                    done->Run();
                return;
            }
#ifdef WEBSERVER_ENABLE_MYSQL
            uint64_t pid = 0;
            std::string err;
            if (!PlayerAccountStore::Instance().RegisterWithPassword(
                    device_id, display_name, hash, salt, PasswordHash::kDefaultIterations, &pid,
                    &err)) {
                rsp->set_ok(false);
                rsp->set_error_code("REGISTER_FAILED");
                rsp->set_message(err);
                if (done)
                    done->Run();
                return;
            }
            rsp->set_ok(true);
            rsp->set_message("ok");
            rsp->set_player_id(pid);
            rsp->set_account_id(pid);
            LOG_WARN << "AuthService Register DEV local MySQL player_id=" << pid;
            if (done)
                done->Run();
            return;
#else
            rsp->set_ok(false);
            rsp->set_error_code("MYSQL_DISABLED");
            rsp->set_message("mysql not enabled");
            if (done)
                done->Run();
            return;
#endif
        }
        gdb::GameDbService_Stub stub(ch);
        gdb::RegisterAccountReq req;
        req.set_device_id(device_id);
        req.set_display_name(display_name);
        req.set_password_hash(hash);
        req.set_password_salt(salt);
        req.set_password_iters(PasswordHash::kDefaultIterations);
        if (idem.empty()) {
            std::string fp;
            if (PasswordHash::Sha256Hex(device_id + "|" + password, &fp))
                idem = "reg:" + device_id + ":" + fp.substr(0, 16);
            else
                idem = "reg:" + device_id;
        }
        req.set_idempotency_key(idem);
        gdb::RegisterAccountRsp grsp;
        brpc::Controller cntl;
        cntl.set_timeout_ms(2000);
        stub.RegisterAccount(&cntl, &req, &grsp, nullptr);
        if (cntl.Failed() || !grsp.ok()) {
            rsp->set_ok(false);
            rsp->set_error_code(grsp.error_code().empty() ? "REGISTER_FAILED" : grsp.error_code());
            rsp->set_message(cntl.Failed() ? cntl.ErrorText() : grsp.message());
            if (done)
                done->Run();
            return;
        }
        rsp->set_ok(true);
        rsp->set_message("ok");
        rsp->set_player_id(grsp.player_id());
        rsp->set_account_id(grsp.account_id());
        LOG_INFO << "AuthService Register ok player_id=" << grsp.player_id()
                 << " device=" << device_id;
        if (done)
            done->Run();
    };
    if (!PasswordExec::Instance().Post(finish))
        finish();
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
    std::string neu_access;
    std::string neu_refresh;
    if (!AuthTokenStore::Instance().RefreshAccessToken(request->player_id(),
                                                       request->refresh_token(), 3600, &neu_access,
                                                       &err, &neu_refresh)) {
        response->set_ok(false);
        response->set_message(err.empty() ? "refresh failed" : err);
        return;
    }
    response->set_ok(true);
    response->set_access_token(neu_access);
    response->set_refresh_token(neu_refresh);
}
