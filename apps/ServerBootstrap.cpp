#include "ServerBootstrap.h"

#include <json/json.h>

#include "AsyncLogging.h"
#include "EventLoop.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpServer.h"
#include "Logging.h"
#include "ProcSelfStats.h"
#include "PrometheusClient.h"
#include "PrometheusMetrics.h"
#include "ResourceWatchdog.h"
#include "FormalMode.h"
#include "GameMeshPaths.h"
#include "ServiceHealth.h"

#include <csignal>
#include <memory>

#ifdef WEBSERVER_ENABLE_MYSQL
#include "ConnectionPool.h"
#include "MetricsDbWriter.h"
#include "PlayerItemPersistQueue.h"
#include "PlayerItemStore.h"
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#include "MailExpireScanner.h"
#include "MailService.h"
#endif
#endif
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#include "GameTcpGateway.h"
#endif
#ifdef WEBSERVER_ENABLE_REDIS
#include "PlacementStore.h"
#include "SessionStore.h"
#endif
#ifdef WEBSERVER_ENABLE_ROCKSDB
#include "DemoKvStore.h"
#endif
#ifdef WEBSERVER_ENABLE_BRPC
#include "BrpcGameServer.h"
#include "BrpcTransport.h"
#include "GameLogicBrpcServer.h"
#include "GatewayConfigPath.h"
#include "WorldBrpcServer.h"
#include "SessionBrpcServer.h"
#include "SessionRpcClient.h"
#include "GatewayAuthClients.h"
#include "GatewayPushServer.h"
#include "GatewayPushClient.h"
#include "IServiceRegistry.h"
#include "GameDbBrpcServer.h"
#include "BrpcGameDbRepository.h"
#include "EtcdDiscovery.h"
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
#include "AsyncMysqlGameDbRepository.h"
#include "PlayerAccountStore.h"
#endif
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#include "InProcessTransport.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>


AsyncLogging *asynclog = nullptr;

void AsyncOutputFunc(const char *msg, int len) {
    if (asynclog)
        asynclog->Append(msg, len);
}

void AsyncFlushFunc() {
    if (asynclog)
        asynclog->Flush();
}

namespace {

constexpr size_t kFileMaximumSize = 100 * 1024 * 1024;
int g_game_port = 0;

std::string ReadFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void SendHtml(HttpResponse *response, const std::string &body, bool head_only) {
    response->SetStatusCode(HttpResponse::HttpStatusCode::k200K);
    response->SetStatusMessage("OK");
    response->SetContentType("text/html; charset=utf-8");
    response->SetContentLength(static_cast<int>(head_only ? 0 : body.size()));
    response->SetBody(head_only ? "" : body);
}

void SendPlain(HttpResponse *response, const std::string &body,
               HttpResponse::HttpStatusCode code = HttpResponse::HttpStatusCode::k200K) {
    response->SetStatusCode(code);
    response->SetStatusMessage(code == HttpResponse::HttpStatusCode::k200K ? "OK" : "Error");
    response->SetContentType("text/plain; charset=utf-8");
    response->SetContentLength(static_cast<int>(body.size()));
    response->SetBody(body);
}

void SendJson(HttpResponse *response, const Json::Value &v,
              HttpResponse::HttpStatusCode code = HttpResponse::HttpStatusCode::k200K) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    const std::string body = Json::writeString(b, v);
    response->SetStatusCode(code);
    response->SetStatusMessage("OK");
    response->SetContentType("application/json; charset=utf-8");
    response->SetContentLength(static_cast<int>(body.size()));
    response->SetBody(body);
}

void SendPrometheusMetrics(HttpResponse *response, const std::string &body, bool head_only) {
    response->SetStatusCode(HttpResponse::HttpStatusCode::k200K);
    response->SetStatusMessage("OK");
    response->SetContentType("text/plain; version=0.0.4; charset=utf-8");
    response->SetContentLength(static_cast<int>(head_only ? 0 : body.size()));
    response->SetBody(head_only ? "" : body);
}

std::string UrlParam(const HttpRequest &request, const char *key) {
    const auto &m = request.request_params();
    const auto it = m.find(key);
    return it != m.end() ? it->second : "";
}

std::string UrlDecodeParam(std::string s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+')
            out.push_back(' ');
        else if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else
                out.push_back(s[i]);
        } else
            out.push_back(s[i]);
    }
    return out;
}

void HandlePrometheusInstant(const HttpRequest &request, HttpResponse *response, bool head_only) {
    std::string q = UrlDecodeParam(UrlParam(request, "q"));
    if (q.empty())
        q = UrlDecodeParam(UrlParam(request, "query"));
    if (q.empty()) {
        SendPlain(response, "missing query param q", HttpResponse::HttpStatusCode::k400BadRequest);
        return;
    }
    const auto r = PrometheusInstantQuery(q);
    if (!r.error.empty()) {
        SendPlain(response, r.error, HttpResponse::HttpStatusCode::k500internalServerError);
        return;
    }
    response->SetStatusCode(static_cast<HttpResponse::HttpStatusCode>(r.http_status));
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->SetContentLength(static_cast<int>(head_only ? 0 : r.body.size()));
    response->SetBody(head_only ? "" : r.body);
}

void HandlePrometheusRange(const HttpRequest &request, HttpResponse *response, bool head_only) {
    std::string q = UrlDecodeParam(UrlParam(request, "q"));
    if (q.empty())
        q = UrlDecodeParam(UrlParam(request, "query"));
    const std::string start = UrlDecodeParam(UrlParam(request, "start"));
    const std::string end = UrlDecodeParam(UrlParam(request, "end"));
    const std::string step = UrlDecodeParam(UrlParam(request, "step"));
    if (q.empty() || start.empty() || end.empty() || step.empty()) {
        SendPlain(response, "need q,start,end,step", HttpResponse::HttpStatusCode::k400BadRequest);
        return;
    }
    const auto r = PrometheusQueryRange(q, start, end, step);
    if (!r.error.empty()) {
        SendPlain(response, r.error, HttpResponse::HttpStatusCode::k500internalServerError);
        return;
    }
    response->SetStatusCode(static_cast<HttpResponse::HttpStatusCode>(r.http_status));
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->SetContentLength(static_cast<int>(head_only ? 0 : r.body.size()));
    response->SetBody(head_only ? "" : r.body);
}


#if 0  // 故意崩溃样例（未接入路由）；保留备查，避免 -Werror 阻断正式构建
void TestHandlePrometheusRange(const HttpRequest &request, HttpResponse *response, bool head_only) {
    std::string q = UrlDecodeParam(UrlParam(request, "q"));
    if (q.empty())
        q = UrlDecodeParam(UrlParam(request, "query"));
    const std::string start = UrlDecodeParam(UrlParam(request, "start"));
    const std::string end = UrlDecodeParam(UrlParam(request, "end"));
    const std::string step = UrlDecodeParam(UrlParam(request, "step"));
    if (q.empty() || start.empty() || end.empty() || step.empty()) {
        SendPlain(response, "need q,start,end,step", HttpResponse::HttpStatusCode::k400BadRequest);
        return;
    }
    const auto r = PrometheusQueryRange(q, start, end, step);
    if (!r.error.empty()) {
        SendPlain(response, r.error, HttpResponse::HttpStatusCode::k500internalServerError);
        return;
    }
    response->SetStatusCode(static_cast<HttpResponse::HttpStatusCode>(r.http_status));
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->SetContentLength(static_cast<int>(head_only ? 0 : r.body.size()));
    response->SetBody(head_only ? "" : r.body);
    int *p = nullptr;
    *p = 1;
}

enum class DumpType {
    NullPointer = 1,
    DivideByZero,
    BufferOverflow,
    UseAfterFree,
    DoubleFree,
    OutOfBounds,
    Abort,
    AssertFail,
    StackOverflow
};

void RecursiveStackOverflow() {
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));

    RecursiveStackOverflow();
}

void TestCppDump(DumpType type) {
    std::cout << "TestCppDump type = " << static_cast<int>(type) << std::endl;

    switch (type) {
    case DumpType::NullPointer: {
        // 空指针解引用，常见宕机原因
        int* p = nullptr;
        *p = 123;
        break;
    }

    case DumpType::DivideByZero: {
        // 整数除 0
        volatile int a = 10;
        volatile int b = 0;
        volatile int c = a / b;
        std::cout << c << std::endl;
        break;
    }

    case DumpType::BufferOverflow: {
        // 栈缓冲区溢出
        char buffer[8];
        strcpy(buffer, "this string is too long");
        std::cout << buffer << std::endl;
        break;
    }

    case DumpType::UseAfterFree: {
        // 释放后继续使用
        int* p = new int(100);
        delete p;

        *p = 200;
        std::cout << *p << std::endl;
        break;
    }

    case DumpType::DoubleFree: {
        // 重复释放
        int* p = new int(123);
        delete p;
        delete p;
        break;
    }

    case DumpType::OutOfBounds: {
        // 数组越界访问
        int arr[3] = {1, 2, 3};
        arr[100] = 999;
        std::cout << arr[100] << std::endl;
        break;
    }

    case DumpType::Abort: {
        // 主动触发 abort，通常会产生 core dump
        std::abort();
        break;
    }

    case DumpType::AssertFail: {
        // 断言失败
        assert(false && "Test assert crash");
        break;
    }

    case DumpType::StackOverflow: {
        // 递归导致栈溢出
        RecursiveStackOverflow();
        break;
    }

    default:
        std::cout << "Unknown dump type" << std::endl;
        break;
    }
}
#endif  // 故意崩溃样例

std::mutex g_gm_sess_mu;
std::unordered_set<std::string> g_gm_sessions;

std::string NewGmSessionId() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    for (int t = 0; t < 100; ++t) {
        std::string s(32, '0');
        for (char &c : s)
            c = hex[gen() & 0xf];
        std::lock_guard<std::mutex> lk(g_gm_sess_mu);
        if (!g_gm_sessions.count(s)) {
            g_gm_sessions.insert(s);
            return s;
        }
    }
    return "gm_fallback";
}

bool GmSessionValid(const std::string &sid) {
    if (sid.empty())
        return false;
    std::lock_guard<std::mutex> lk(g_gm_sess_mu);
    return g_gm_sessions.count(sid) > 0;
}

std::string GmSidFromRequest(const HttpRequest &request) {
    const std::string cookie = request.GetHeader("Cookie");
    const std::string key = "gm_sid=";
    const auto p = cookie.find(key);
    if (p == std::string::npos)
        return "";
    const auto end = cookie.find(';', p);
    return cookie.substr(p + key.size(), end == std::string::npos ? std::string::npos : end - p - key.size());
}

void FindAllFiles(const std::string &dir, std::vector<std::string> *out) {
    DIR *d = ::opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *ent = ::readdir(d)) {
        if (ent->d_name[0] == '.')
            continue;
        out->push_back(ent->d_name);
    }
    ::closedir(d);
}

Json::Value BuildFilesJson() {
    std::vector<std::string> names;
    FindAllFiles(GameMeshPaths::FilesRoot(), &names);
    Json::Value arr(Json::arrayValue);
    for (const auto &n : names)
        arr.append(n);
    return arr;
}

std::string BuildFileHtml() {
    std::ostringstream os;
    os << "<html><body><h3>Files</h3><ul>";
    std::vector<std::string> names;
    FindAllFiles(GameMeshPaths::FilesRoot(), &names);
    for (const auto &n : names)
        os << "<li><a href=\"/download?file=" << n << "\">" << n << "</a></li>";
    os << "</ul></body></html>";
    return os.str();
}

bool DownloadFile(const std::string &name, HttpResponse *response, bool head_only) {
    const std::string path = GameMeshPaths::FilesRoot() + "/" + name;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        SendPlain(response, "file not found", HttpResponse::HttpStatusCode::k404NotFound);
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }
    response->SetStatusCode(HttpResponse::HttpStatusCode::k200K);
    response->SetStatusMessage("OK");
    response->SetContentType("application/octet-stream");
    response->SetContentLength(static_cast<int>(st.st_size));
    response->SetBodyType(HttpResponse::HttpBodyType::FILE_TYPE);
    response->SetFileFd(fd);
    if (head_only)
        ::close(fd);
    return true;
}

std::string GmConsolePage(const HttpRequest &, const std::string &sid) {
    std::ostringstream os;
    os << "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>GM</title></head><body>"
       << "<p>gm_sid=" << sid << "</p>"
       << "<form method=\"post\" action=\"/gm\"><input name=\"cmd\" size=\"48\" "
          "placeholder=\"help / stat / watch / hcpu / hcpu_stop\"/> "
       << "<button type=\"submit\">run</button></form>"
       << "<p><a href=\"/\">home</a> | <a href=\"/monitor\">monitor</a></p></body></html>";
    return os.str();
}

Json::Value GmExecute(const std::string &cmd) {
    Json::Value root;
    if (cmd == "help") {
        root["help"] = "stat watch history proc_line hcpu hcpu_stop";
        return root;
    }
    if (cmd == "stat" || cmd == "watch") {
        root["last"] = ResourceWatchdog::Instance().LastSampleJson();
        root["trends"] = ResourceWatchdog::Instance().AnalyzeTrends();
        return root;
    }
    if (cmd == "history") {
        root["history"] = ResourceWatchdog::Instance().HistoryJson(120);
        return root;
    }
    if (cmd == "proc_line") {
        ProcResourceSnapshot s;
        FillProcResourceSnapshot(&s);
        root["line"] = ProcResourceSnapshotFormatLine(s);
        return root;
    }
    root["error"] = "unknown cmd";
    return root;
}

}  // namespace

static int *g_uaf_ptr = nullptr;

void crash_uaf() {
    delete g_uaf_ptr;
    g_uaf_ptr = new int(42);
    *g_uaf_ptr = 1;
    delete g_uaf_ptr;
    g_uaf_ptr = nullptr;
}

void crash_stack_overflow(int depth);

void trigger_stack_overflow() {
    crash_stack_overflow(0);
}

void crash_stack_overflow(int depth) {
    char buf[1024 * 64];
    memset(buf, depth, sizeof(buf));
    crash_stack_overflow(depth + 1);
}

void HttpResponseCallback(const HttpRequest &request, HttpResponse *response) {
    const bool head_only = (request.method() == HttpRequest::Method::kHead);
    const std::string &url = request.url();

    if (url == "/login" && request.method() == HttpRequest::Method::kPost) {
        if (!AdminHttpEnabled()) {
            SendPlain(response, "admin disabled", HttpResponse::HttpStatusCode::k403Forbidden);
            return;
        }
        const std::string user = UrlParam(request, "username");
        const std::string pass = UrlParam(request, "password");
        if (user == "demo" && pass == "demo") {
            const std::string sid = NewGmSessionId();
            response->AddHeader("Set-Cookie", "gm_sid=" + sid + "; Path=/");
            SendHtml(response, "<html><body>login ok <a href=\"/gm\">GM</a></body></html>", head_only);
        } else if (user == "asan" && pass == "asan") {
            // ASan 触发接口默认关闭；需显式 GAMEMESH_ENABLE_ASAN_CRASH=1
            const char *asan = std::getenv("GAMEMESH_ENABLE_ASAN_CRASH");
            if (!(asan && std::strcmp(asan, "1") == 0)) {
                SendPlain(response, "asan crash disabled", HttpResponse::HttpStatusCode::k403Forbidden);
                return;
            }
            crash_uaf();
            SendPlain(response, "never");
        } else {
            SendPlain(response, "login failed", HttpResponse::HttpStatusCode::k403Forbidden);
        }
        return;
    }

    if (url == "/gm") {
        if (!AdminHttpEnabled()) {
            SendPlain(response, "admin disabled", HttpResponse::HttpStatusCode::k403Forbidden);
            return;
        }
        const std::string sid = GmSidFromRequest(request);
        if (!GmSessionValid(sid)) {
            Json::Value e;
            e["error"] = "gm_session_invalid";
            e["hint"] = "POST /login with username=demo password=demo";
            SendJson(response, e, HttpResponse::HttpStatusCode::k403Forbidden);
            return;
        }
        if (request.method() == HttpRequest::Method::kGet) {
            SendHtml(response, GmConsolePage(request, sid), head_only);
            return;
        }
        if (request.method() == HttpRequest::Method::kPost) {
            const std::string cmd = UrlParam(request, "cmd");
            SendJson(response, GmExecute(cmd));
            return;
        }
    }

    if (url == "/metrics") {
        SendPrometheusMetrics(response, BuildPrometheusMetricsText(), head_only);
        return;
    }
    if (url == "/api/prometheus/query") {
        HandlePrometheusInstant(request, response, head_only);
        return;
    }
    if (url == "/api/prometheus/query_range") {
        HandlePrometheusRange(request, response, head_only);
        return;
    }
#ifdef WEBSERVER_ENABLE_MYSQL
    if (url == "/api/db/ping") {
        if (!AdminHttpEnabled()) {
            SendPlain(response, "admin disabled", HttpResponse::HttpStatusCode::k403Forbidden);
            return;
        }
        Json::Value j;
        auto *pool = ConnectionPool::getconnectionPool();
        j["ok"] = pool->isInitialized();
        j["dbname"] = pool->dbname();
        j["table"] = "gamemesh_metrics";
        j["flush_interval_sec"] = 10;
        if (pool->isInitialized()) {
            if (auto conn = pool->getConnection()) {
                MYSQL_RES *res =
                    conn->query("SELECT COUNT(*), MAX(ts_unix) FROM gamemesh_metrics");
                if (res) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    if (row && row[0]) {
                        j["row_count"] = static_cast<Json::UInt64>(std::strtoull(row[0], nullptr, 10));
                        if (row[1])
                            j["last_ts_unix"] = static_cast<Json::Int64>(std::strtoll(row[1], nullptr, 10));
                    }
                    mysql_free_result(res);
                }
            }
        }
        SendJson(response, j);
        return;
    }
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    if (url == "/api/gm/mail/deliver" && request.method() == HttpRequest::Method::kPost) {
        const std::string sid = GmSidFromRequest(request);
        if (!GmSessionValid(sid)) {
            Json::Value e;
            e["ok"] = false;
            e["error_code"] = "PERMISSION_DENIED";
            e["message"] = "gm_session_invalid";
            SendJson(response, e, HttpResponse::HttpStatusCode::k403Forbidden);
            return;
        }
        Json::Value root;
        Json::CharReaderBuilder b;
        std::string errs;
        const std::string &body = request.body();
        std::unique_ptr<Json::CharReader> reader(b.newCharReader());
        Json::Value out;
        if (!reader->parse(body.data(), body.data() + body.size(), &root, &errs)) {
            out["ok"] = false;
            out["error_code"] = "INVALID_ARGUMENT";
            out["message"] = "invalid json: " + errs;
            SendJson(response, out, HttpResponse::HttpStatusCode::k400BadRequest);
            return;
        }
        mail::DeliverRequest d;
        d.source_system = root.get("source_system", "gm").asString();
        d.business_key = root.get("business_key", "").asString();
        d.receiver_type = root.get("receiver_type", "ROLE").asString();
        d.receiver_id = root.get("receiver_id", 0).asUInt64();
        d.template_id = root.get("template_id", "").asString();
        d.template_version = root.get("template_version", 1).asInt();
        d.category = root.get("category", "SYSTEM").asString();
        d.priority = root.get("priority", 0).asInt();
        d.sender_name = root.get("sender_name", "GM").asString();
        d.title = root.get("title", "").asString();
        d.body = root.get("body", "").asString();
        d.send_at = root.get("send_at", 0).asInt64();
        d.expire_at = root.get("expire_at", 0).asInt64();
        d.trace_id = root.get("trace_id", sid).asString();
        if (root.isMember("attachments") && root["attachments"].isArray()) {
            for (const auto &ja : root["attachments"]) {
                mail::DeliverAttachment a;
                a.asset_type = ja.get("asset_type", "ITEM").asString();
                a.asset_id = ja.get("asset_id", 0).asUInt64();
                a.count = ja.get("count", 0).asUInt();
                a.bind_type = ja.get("bind_type", "NONE").asString();
                a.payload = ja.get("payload", "").asString();
                d.attachments.push_back(a);
            }
        }
        uint64_t mail_id = 0;
        std::string ec, msg;
        const bool ok = MailService::Instance().Deliver(d, &mail_id, &ec, &msg);
        out["ok"] = ok;
        out["error_code"] = ec;
        out["message"] = msg;
        out["mail_id"] = static_cast<Json::UInt64>(mail_id);
        out["idempotent_hit"] = ok && msg.find("idempotent") != std::string::npos;
        LOG_INFO << "GM mail deliver actor_sid=" << sid.substr(0, 8)
                 << " receiver=" << d.receiver_id << " mail_id=" << mail_id << " ok=" << ok
                 << " code=" << ec;
        SendJson(response, out, ok ? HttpResponse::HttpStatusCode::k200K
                                   : HttpResponse::HttpStatusCode::k400BadRequest);
        return;
    }
#endif
#endif
    if (url == "/api/health" || url == "/api/liveness") {
        response->SetContentType("application/json");
        SendPlain(response, ServiceHealth::Instance().LivenessJson());
        return;
    }
    if (url == "/api/readiness") {
        const bool deps_ok = !ServiceHealth::Instance().draining();
        const std::string detail = deps_ok ? "ok" : "draining";
        response->SetContentType("application/json");
        SendPlain(response, ServiceHealth::Instance().ReadinessJson(deps_ok, detail),
                  deps_ok ? HttpResponse::HttpStatusCode::k200K
                           : HttpResponse::HttpStatusCode::k503ServiceUnavailable);
        return;
    }
    if (url == "/api/version") {
        response->SetContentType("application/json");
        SendPlain(response, ServiceHealth::Instance().VersionJson());
        return;
    }
    if (url == "/api/files") {
        SendJson(response, BuildFilesJson());
        return;
    }
    if (url == "/api/stats") {
        Json::Value j;
        j["watchdog"] = ResourceWatchdog::Instance().LastSampleJson();
        const std::string hist = UrlParam(request, "history");
        if (!hist.empty())
            j["history"] = ResourceWatchdog::Instance().HistoryJson(std::atoi(hist.c_str()));
        SendJson(response, j);
        return;
    }
    if (url == "/api/game/status") {
        Json::Value j;
        j["enabled"] = g_game_port > 0;
        j["port"] = g_game_port;
        SendJson(response, j);
        return;
    }
#ifdef WEBSERVER_ENABLE_REDIS
    if (url == "/api/redis/status") {
        Json::Value j;
        j["enabled"] = true;
        j["available"] = SessionStore::Instance().Available();
        SendJson(response, j);
        return;
    }
#endif
    if (url == "/api/echo") {
        Json::Value j;
        j["url"] = url;
        j["body"] = request.body();
        SendJson(response, j);
        return;
    }
    if (url == "/download") {
        const std::string file = UrlParam(request, "file");
        if (!file.empty())
            DownloadFile(file, response, head_only);
        else
            SendPlain(response, "missing file", HttpResponse::HttpStatusCode::k400BadRequest);
        return;
    }
    if (url == "/delete" && request.method() == HttpRequest::Method::kPost) {
        const std::string file = UrlParam(request, "file");
        const std::string path = GameMeshPaths::FilesRoot() + "/" + file;
        const int rc = ::remove(path.c_str());
        Json::Value j;
        j["removed"] = (rc == 0);
        SendJson(response, j);
        return;
    }

    if (url == "/" || url == "/index.html") {
        std::string body = ReadFile(GameMeshPaths::StaticRoot() + "/index.html");
        if (body.empty())
            body = "<html><body><h3>GameMesh</h3><ul>"
                   "<li><a href=\"/monitor\">monitor</a></li>"
                   "<li><a href=\"/metrics\">metrics</a></li>"
                   "<li><a href=\"/fileserver\">fileserver</a></li>"
                   "<li><a href=\"/gm\">gm</a></li></ul></body></html>";
        SendHtml(response, body, head_only);
        return;
    }
    if (url == "/monitor" || url == "/monitor.html") {
        std::string body = ReadFile(GameMeshPaths::StaticRoot() + "/monitor.html");
        if (body.empty())
            body = "<html><body>缺少 static/monitor.html</body></html>";
        SendHtml(response, body, head_only);
        return;
    }
    if (url == "/fileserver" || url == "/fileserver.html") {
        std::string body = ReadFile(GameMeshPaths::StaticRoot() + "/fileserver.html");
        if (body.empty())
            body = BuildFileHtml();
        SendHtml(response, body, head_only);
        return;
    }
    if (url == "/mhw" || url == "/mhw.html") {
        SendHtml(response, ReadFile(GameMeshPaths::StaticRoot() + "/mhw.html"), head_only);
        return;
    }
    if (url == "/about") {
        SendHtml(response, "<html><body><h3>GameMesh</h3></body></html>", head_only);
        return;
    }

    if (url.size() > 1 && url[0] == '/') {
        std::string path = GameMeshPaths::StaticRoot() + url;
        std::string body = ReadFile(path);
        if (!body.empty()) {
            SendHtml(response, body, head_only);
            return;
        }
    }

    response->SetStatusCode(HttpResponse::HttpStatusCode::k404NotFound);
    response->SetStatusMessage("Not Found");
    response->SetCloseConnection(true);
}

bool ParseLaunchArgs(int argc, char *argv[], LaunchOpts *opts) {
    if (!opts)
        return false;
    auto is_role = [](const char *s) {
        return s && (!strcmp(s, "all") || !strcmp(s, "gateway") || !strcmp(s, "gamelogic") ||
                     !strcmp(s, "world") || !strcmp(s, "session") || !strcmp(s, "gamedb"));
    };

    std::string role = opts->force_role.empty() ? "all" : opts->force_role;
    int port = opts->http_port > 0 ? opts->http_port : 8080;
    int game_port = opts->game_port;
    int logic_port_override = opts->logic_port_override;

    int argi = 1;
    if (opts->force_role.empty()) {
        if (argc >= 2 && is_role(argv[1])) {
            role = argv[1];
            argi = 2;
        }
    }
    if (argi < argc)
        port = std::atoi(argv[argi++]);
    if (argi < argc) {
        if (role == "gamelogic" || role == "session" || role == "gamedb")
            logic_port_override = std::atoi(argv[argi]);
        else if (role != "world")
            game_port = std::atoi(argv[argi]);
    } else if (game_port <= 0 && role != "gamelogic" && role != "world" && role != "session" &&
               role != "gamedb") {
        game_port = port + 1;
    }

    opts->role = role;
    opts->http_port = port;
    opts->game_port = game_port;
    opts->logic_port_override = logic_port_override;
    return true;
}

int RunServer(const LaunchOpts &launch) {
    LaunchOpts opts = launch;
    Logger::setLogLevel(Logger::INFO);
    ::mkdir("./log", 0755);
    const std::string log_path = "./log/" + opts.log_basename + ".log";
    asynclog = new AsyncLogging(log_path.c_str());
    asynclog->Start();
    Logger::setOutput(AsyncOutputFunc);
    Logger::setFlush(AsyncFlushFunc);

    const std::string &role = opts.role;
    const int port = opts.http_port;
    int game_port = opts.game_port;
    const int logic_port_override = opts.logic_port_override;
    if (game_port <= 0 && role != "gamelogic" && role != "world" && role != "session" &&
        role != "gamedb")
        game_port = port + 1;

    if (role == "gamelogic" || role == "world" || role == "session" || role == "gamedb")
        g_game_port = 0;
    else
        g_game_port = game_port;

    LOG_INFO << "gamemesh role=" << role << " http=" << port << " game_port=" << g_game_port;
    if (FormalModeEnabled()) {
        LOG_WARN << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!";
        LOG_WARN << " GAMEMESH_FORMAL=1: fail-closed ownership enforced";
        LOG_WARN << " Auth requires GameDB; Register via Auth→GameDB only";
        LOG_WARN << " Admin HTTP disabled unless GAMEMESH_ENABLE_ADMIN=1";
        LOG_WARN << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!";
    }
    if (role == "all" && FormalModeEnabled()) {
        LOG_WARN << "role=all under FORMAL is for emergency only; prefer multi-process topology";
    }
    ServiceHealth::Instance().Configure(role, role + "-" + std::to_string(port));

#if !defined(WEBSERVER_ENABLE_BRPC)
    if (role == "gateway" || role == "gamelogic" || role == "world" || role == "session" ||
        role == "gamedb") {
        LOG_ERROR << "role=" << role << " requires cmake -DENABLE_BRPC=ON";
        return 1;
    }
#endif

    EventLoop loop;
    static EventLoop *g_main_loop = nullptr;
    g_main_loop = &loop;
    std::signal(SIGTERM, [](int) {
        ServiceHealth::Instance().SetDraining(true);
        if (g_main_loop)
            g_main_loop->Quit();
    });
    std::signal(SIGINT, [](int) {
        ServiceHealth::Instance().SetDraining(true);
        if (g_main_loop)
            g_main_loop->Quit();
    });
#ifdef WEBSERVER_ENABLE_MYSQL
    const bool need_db =
        (role == "all" || role == "gamelogic" || role == "world" || role == "gamedb");
    if (need_db && ConnectionPool::getconnectionPool()->isInitialized()) {
        if (role != "gamedb")
            MetricsDbWriter::Instance().StartPeriodic(&loop, 10.0);
        if (role != "gamedb" && role != "world") {
            PlayerItemStore::Instance().EnsureTable();
            PlayerAccountStore::Instance().EnsureTable();
            PlayerItemPersistQueue::Instance().StartPeriodic(&loop, 300.0);
        } else if (role == "world") {
            PlayerItemStore::Instance().EnsureTable();
        }
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
        // world：若配置 gamedb_addrs，先挂 BrpcGameDb 再 Init MailService（见下方 brpc 分支）
        if (role == "all" || role == "gamelogic") {
            if (MailService::Instance().Init()) {
                if (role == "all")
                    MailExpireScanner::Instance().StartPeriodic(&loop, 0.0);
            } else {
                LOG_WARN << "MailService init failed";
            }
        }
#endif
    } else if (need_db) {
        LOG_WARN << "MySQL pool not initialized (config/mysql.cnf)";
    }
#endif
#ifdef WEBSERVER_ENABLE_REDIS
    if (role == "all" || role == "gamelogic" || role == "gateway" || role == "world" ||
        role == "session") {
        if (!SessionStore::Instance().InitFromConfig())
            LOG_WARN << "Redis session disabled (config/redis.cnf)";
        else if (role == "all" || role == "session") {
            PlacementStore::Instance().InitFromSessionPrefix(SessionStore::Instance().key_prefix());
        }
    }
#endif
#ifdef WEBSERVER_ENABLE_ROCKSDB
    if (role == "all") {
        if (!DemoKvStore::Instance().InitFromConfig())
            LOG_WARN << "RocksDB demo KV disabled (config/rocksdb.cnf)";
    }
#endif
    ResourceWatchdog::Instance().StartPeriodic(&loop, 5.0);

    const unsigned ncpu = std::thread::hardware_concurrency();
    const int size = ncpu > 0 ? static_cast<int>(ncpu) : 4;
    const char *http_bind_env = std::getenv("GAMEMESH_HTTP_BIND");
    std::string http_bind = (http_bind_env && *http_bind_env) ? http_bind_env : "0.0.0.0";
    if (FormalModeEnabled() && (!http_bind_env || !*http_bind_env))
        http_bind = "127.0.0.1";  // 正式模式默认管理口仅 loopback
    HttpServer server(&loop, http_bind.c_str(), port, true);
    server.SetHttpCallback(HttpResponseCallback);
    server.SetThreadNums(size);
    LOG_INFO << "HTTP admin bind=" << http_bind << ":" << port
             << " admin_apis=" << (AdminHttpEnabled() ? "on" : "off");

#ifdef WEBSERVER_ENABLE_BRPC
    auto load_kv = [](const std::string &path, const char *key) -> std::string {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = line.substr(0, eq);
            while (!k.empty() && k.back() == ' ')
                k.pop_back();
            if (k == key) {
                std::string v = line.substr(eq + 1);
                while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' '))
                    v.pop_back();
                size_t i = 0;
                while (i < v.size() && v[i] == ' ')
                    ++i;
                return v.substr(i);
            }
        }
        return {};
    };

    auto split_addrs = [](const std::string &csv) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : csv) {
            if (c == ',') {
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                size_t i = 0;
                while (i < cur.size() && cur[i] == ' ') ++i;
                cur = cur.substr(i);
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else
                cur.push_back(c);
        }
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\r')) cur.pop_back();
        size_t i = 0;
        while (i < cur.size() && cur[i] == ' ') ++i;
        cur = cur.substr(i);
        if (!cur.empty()) out.push_back(cur);
        return out;
    };

    {
        const std::string etcd = GatewayConfigPath::ReadValue("etcd_endpoints");
        if (!etcd.empty())
            EtcdDiscovery::Instance().Configure(etcd);
        else
            LOG_WARN << "etcd_endpoints empty; discovery disabled (static *.cnf)";
    }

    if (role == "session") {
        if (logic_port_override > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "0.0.0.0:%d", logic_port_override);
            if (!SessionBrpcServer::Instance().Start(buf, 30)) {
                LOG_ERROR << "SessionBrpcServer start failed";
                return 1;
            }
        } else if (!SessionBrpcServer::Instance().StartFromConfig()) {
            LOG_ERROR << "SessionBrpcServer start failed";
            return 1;
        }
        EtcdDiscovery::Instance().Register("session", "session-1",
                                           SessionBrpcServer::Instance().listen_addr(), 30);
        LOG_INFO << "role=session listen=" << SessionBrpcServer::Instance().listen_addr();
    } else if (role == "gamedb") {
        if (logic_port_override > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "0.0.0.0:%d", logic_port_override);
            if (!GameDbBrpcServer::Instance().Start(buf, 30)) {
                LOG_ERROR << "GameDbBrpcServer start failed";
                return 1;
            }
        } else if (!GameDbBrpcServer::Instance().StartFromConfig()) {
            LOG_ERROR << "GameDbBrpcServer start failed";
            return 1;
        }
        const char *gid = (logic_port_override == 8501 || logic_port_override <= 0) ? "gamedb-0" : "gamedb-1";
        EtcdDiscovery::Instance().Register("gamedb", gid,
                                           GameDbBrpcServer::Instance().listen_addr(), 30);
        LOG_INFO << "role=gamedb listen=" << GameDbBrpcServer::Instance().listen_addr();
    } else if (role == "gamelogic") {
        std::string listen = "0.0.0.0:8201";
        if (logic_port_override > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "0.0.0.0:%d", logic_port_override);
            listen = buf;
        }
        if (logic_port_override > 0) {
            const char *iid = (logic_port_override == 8201) ? "gl-0" : "gl-1";
            MapInstanceRegistry::Instance().SetLocalInstanceId(iid);
            if (!GameLogicBrpcServer::Instance().Start(listen, 30)) {
                LOG_ERROR << "GameLogicBrpcServer start failed";
                return 1;
            }
        } else if (!GameLogicBrpcServer::Instance().StartFromConfig()) {
            LOG_ERROR << "GameLogicBrpcServer start failed";
            return 1;
        }
        {
            std::string session_addr = GatewayConfigPath::ReadValue("session_addrs");
            if (session_addr.empty()) {
                std::string logic_cnf = "../config/gamelogic.cnf";
                std::string resolved;
                if (GameMeshPaths::ResolveProjectSubdir("config/gamelogic.cnf", &resolved))
                    logic_cnf = resolved;
                session_addr = load_kv(logic_cnf, "session_addrs");
            }
            if (!session_addr.empty()) {
                const auto comma = session_addr.find(',');
                if (comma != std::string::npos)
                    session_addr = session_addr.substr(0, comma);
                SessionRpcClient::Instance().Init(session_addr);
            }
            std::string gamedb_addr = GatewayConfigPath::ReadValue("gamedb_addrs");
            if (gamedb_addr.empty()) {
                std::string logic_cnf = "../config/gamelogic.cnf";
                std::string resolved;
                if (GameMeshPaths::ResolveProjectSubdir("config/gamelogic.cnf", &resolved))
                    logic_cnf = resolved;
                gamedb_addr = load_kv(logic_cnf, "gamedb_addrs");
            }
            if (!gamedb_addr.empty()) {
                auto addrs = split_addrs(gamedb_addr);
                BrpcGameDbRepository::Instance().Init(addrs);
            }
            // Push 端点：从配置 gateway_push_addrs=id=host:port,... 或服务发现；禁止写死端口
            {
                std::string push_map = GatewayConfigPath::ReadValue("gateway_push_addrs");
                if (push_map.empty()) {
                    std::string logic_cnf = "../config/gamelogic.cnf";
                    std::string resolved;
                    if (GameMeshPaths::ResolveProjectSubdir("config/gamelogic.cnf", &resolved))
                        logic_cnf = resolved;
                    push_map = load_kv(logic_cnf, "gateway_push_addrs");
                }
                auto entries = split_addrs(push_map);  // 复用 CSV；项为 id=addr
                for (const auto &e : entries) {
                    const auto eq = e.find('=');
                    if (eq == std::string::npos)
                        continue;
                    const std::string id = e.substr(0, eq);
                    const std::string addr = e.substr(eq + 1);
                    GatewayPushClient::Instance().SetGatewayPushAddr(id, addr);
                    IServiceRegistry::ServiceInstance inst;
                    inst.service = "gateway_push";
                    inst.instance_id = id;
                    inst.address = addr;
                    inst.status = "UP";
                    StaticServiceRegistry::Get().RegisterInstance(inst);
                }
            }
            const char *adv = std::getenv("GAMEMESH_ADVERTISE_HOST");
            std::string logic_adv = (adv && *adv) ? adv : "127.0.0.1";
            std::string logic_listen = GameLogicBrpcServer::Instance().listen_addr();
            std::string logic_port = "8201";
            const auto colon = logic_listen.rfind(':');
            if (colon != std::string::npos)
                logic_port = logic_listen.substr(colon + 1);
            EtcdDiscovery::Instance().Register(
                "gamelogic", MapInstanceRegistry::Instance().local_instance_id(),
                logic_adv + ":" + logic_port, 30);
        }
    } else if (role == "gateway") {
        std::vector<std::string> addrs;
        std::vector<std::string> ids;
        std::vector<std::string> world_addrs;
        int timeout_ms = 3000;
        if (!GatewayConfigPath::Load(&addrs, &ids, &world_addrs, &timeout_ms)) {
            addrs = {"127.0.0.1:8201"};
            ids = {"gl-0"};
            world_addrs = {"127.0.0.1:8301"};
            LOG_WARN << "gateway.cnf missing, default logic+world addrs";
        }
        if (EtcdDiscovery::Instance().enabled()) {
            std::vector<std::string> discovered;
            if (EtcdDiscovery::Instance().Discover("gamelogic", &discovered) && !discovered.empty()) {
                addrs = discovered;
                LOG_INFO << "etcd discovered gamelogic addrs=" << addrs.size();
            } else {
                LOG_WARN << "etcd gamelogic discover failed; using static logic_addrs";
            }
            if (EtcdDiscovery::Instance().Discover("world", &discovered) && !discovered.empty()) {
                world_addrs = discovered;
                LOG_INFO << "etcd discovered world addrs=" << world_addrs.size();
            }
            if (EtcdDiscovery::Instance().Discover("session", &discovered) && !discovered.empty()) {
                SessionRpcClient::Instance().Init(discovered[0], timeout_ms);
            }
        }
        if (!BrpcTransport::Instance().EnsureStarted(addrs, ids, world_addrs, timeout_ms)) {
            LOG_ERROR << "BrpcTransport init failed";
            return 1;
        }
        if (!SessionRpcClient::Instance().ready()) {
            std::string session_addr = GatewayConfigPath::ReadValue("session_addrs");
            if (!session_addr.empty()) {
                const auto comma = session_addr.find(',');
                if (comma != std::string::npos)
                    session_addr = session_addr.substr(0, comma);
                SessionRpcClient::Instance().Init(session_addr, timeout_ms);
            } else {
                LOG_INFO << "session_addrs empty; gateway uses local SessionStore";
            }
        }
        // Auth/Session 编排客户端 + Logic Bind/Dispatch Channel（复用，非逐请求创建）
        {
            std::string session_addr = GatewayConfigPath::ReadValue("session_addrs");
            if (!session_addr.empty()) {
                const auto comma = session_addr.find(',');
                if (comma != std::string::npos)
                    session_addr = session_addr.substr(0, comma);
                GatewayAuthClients::Instance().InitAuthSession(session_addr, timeout_ms);
            }
            GatewayAuthClients::Instance().InitLogicChannels(addrs, ids, timeout_ms);
            StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", addrs, ids);
            StaticServiceRegistry::Get().SetStaticAddrs(
                "session", {GatewayConfigPath::ReadValue("session_addrs")});
            StaticServiceRegistry::Get().SetStaticAddrs("world", world_addrs);
        }
        // GatewayPush 内网口：默认 game_port+100；注册 advertise 地址（非 0.0.0.0）
        {
            const int push_port = (game_port > 0 ? game_port : port) + 100;
            char listen_buf[64];
            snprintf(listen_buf, sizeof(listen_buf), "0.0.0.0:%d", push_port);
            const std::string gw_id = "gw-" + std::to_string(game_port > 0 ? game_port : port);
            GatewayPushServer::Instance().set_gateway_instance_id(gw_id);
            if (!GatewayPushServer::Instance().Start(listen_buf))
                LOG_WARN << "GatewayPushServer start failed " << listen_buf;
            const char *adv = std::getenv("GAMEMESH_ADVERTISE_HOST");
            std::string host = (adv && *adv) ? adv : "127.0.0.1";
            char adv_buf[96];
            snprintf(adv_buf, sizeof(adv_buf), "%s:%d", host.c_str(), push_port);
            EtcdDiscovery::Instance().Register("gateway_push", gw_id, adv_buf, 30);
            IServiceRegistry::ServiceInstance inst;
            inst.service = "gateway_push";
            inst.instance_id = gw_id;
            inst.address = adv_buf;
            inst.port = push_port;
            inst.status = "UP";
            StaticServiceRegistry::Get().RegisterInstance(inst);
            GatewayPushClient::Instance().SetGatewayPushAddr(gw_id, adv_buf);
        }
        {
            const char *adv = std::getenv("GAMEMESH_ADVERTISE_HOST");
            std::string host = (adv && *adv) ? adv : "127.0.0.1";
            const int gp = game_port > 0 ? game_port : port;
            EtcdDiscovery::Instance().Register("gateway", "gw-" + std::to_string(gp),
                                              host + ":" + std::to_string(gp), 30);
        }
    } else if (role == "world") {
        {
            std::string gamedb_addr = GatewayConfigPath::ReadValue("gamedb_addrs");
            if (gamedb_addr.empty()) {
                std::string world_cnf = "../config/world.cnf";
                std::string resolved;
                if (GameMeshPaths::ResolveProjectSubdir("config/world.cnf", &resolved))
                    world_cnf = resolved;
                gamedb_addr = load_kv(world_cnf, "gamedb_addrs");
            }
            if (!gamedb_addr.empty()) {
                auto addrs = split_addrs(gamedb_addr);
                BrpcGameDbRepository::Instance().Init(addrs);
            }
        }
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#ifdef WEBSERVER_ENABLE_MYSQL
        if (MailService::Instance().Init())
            MailExpireScanner::Instance().StartPeriodic(&loop, 0.0);
        else
            LOG_WARN << "MailService init failed";
#endif
#endif
        if (!WorldBrpcServer::Instance().StartFromConfig()) {
            LOG_ERROR << "WorldBrpcServer start failed";
            return 1;
        }
        EtcdDiscovery::Instance().Register("world", "world-1",
                                           WorldBrpcServer::Instance().listen_addr(), 30);
    }
#endif

#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    if (role == "all" || role == "gateway") {
        if (role == "all") {
            InProcessTransport::Instance().EnsureStarted(0);
            MapInstanceRegistry::Instance().SetLocalInstanceId("gl-local");
            MapPlacement::Instance().ConfigureOwners({"gl-local"});
        }
        auto *gw = new GameTcpGateway("0.0.0.0", game_port);
        gw->StartInBackground();
        LOG_INFO << "Game protobuf TCP " << game_port << " (HTTP " << port << ") role=" << role;
    }
#endif

#ifdef WEBSERVER_ENABLE_BRPC
    if (role == "all") {
        if (!BrpcGameServer::Instance().StartFromConfig())
            LOG_WARN << "brpc MailBrpcService disabled (config/brpc.cnf or bind failed)";
    }
#endif
    ServiceHealth::Instance().SetReady(true);
    ServiceHealth::Instance().MarkAlive();
    server.start();
    ServiceHealth::Instance().SetDraining(true);
    ServiceHealth::Instance().SetReady(false);
    LOG_INFO << "graceful shutdown begin role=" << role;
#ifdef WEBSERVER_ENABLE_BRPC
    if (role == "session" || role == "all")
        SessionBrpcServer::Instance().Stop();
    if (role == "gamelogic" || role == "all")
        GameLogicBrpcServer::Instance().Stop();
    if (role == "world" || role == "all")
        WorldBrpcServer::Instance().Stop();
    if (role == "gamedb" || role == "all")
        GameDbBrpcServer::Instance().Stop();
    if (role == "gateway" || role == "all")
        GatewayPushServer::Instance().Stop();
#endif
    LOG_INFO << "graceful shutdown done role=" << role;
    return 0;
}
