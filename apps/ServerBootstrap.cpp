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
#include "GatewayIdentity.h"
#include "HealthDeps.h"
#include "HealthProbe.h"
#include "MapLeaseKeeper.h"
#include "PlacementRecoveryScheduler.h"
#include "OpsMetrics.h"
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
#include "GameLogic.h"
#include "MapCatalog.h"
#endif
#ifdef WEBSERVER_ENABLE_REDIS
#include "PlacementStore.h"
#include "PushReplayStore.h"
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
#include "AdvertiseAddr.h"
#include "BrpcChannelManager.h"
#include "GameDbBrpcServer.h"
#include "BrpcGameDbRepository.h"
#include "EtcdDiscovery.h"
#include "RedisServiceRegistry.h"
#include "HealthyLogicOwners.h"
#include "AuthLoginRateLimit.h"
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
#include "AsyncMysqlGameDbRepository.h"
#include "GameDbOutbox.h"
#include "PlayerAccountStore.h"
#include "PlayerProfileStore.h"
#include "GameDbAssetStore.h"
#endif
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#include "InProcessTransport.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

std::string InventoryTsvPath() {
    if (const char *run = std::getenv("GAMEMESH_RUN_DIR")) {
        if (*run)
            return std::string(run) + "/inventory.tsv";
    }
    std::string formal;
    if (GameMeshPaths::ResolveProjectSubdir("run/formal", &formal))
        return formal + "/inventory.tsv";
    return "";
}

bool PidAlive(int pid) {
    if (pid <= 0)
        return false;
    return ::kill(pid, 0) == 0;
}

std::string ProcComm(int pid) {
    std::string s = ReadFile("/proc/" + std::to_string(pid) + "/comm");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

int64_t ProcRssKb(int pid) {
    const std::string st = ReadFile("/proc/" + std::to_string(pid) + "/status");
    const auto pos = st.find("VmRSS:");
    if (pos == std::string::npos)
        return 0;
    return std::strtoll(st.c_str() + pos + 6, nullptr, 10);
}

int HttpGetLocal(int port, const char *path, int timeout_ms, std::string *body) {
    if (port <= 0 || !path)
        return 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    const std::string s = req.str();
    if (::send(fd, s.data(), s.size(), 0) < 0) {
        ::close(fd);
        return 0;
    }
    std::string raw;
    char buf[2048];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        raw.append(buf, static_cast<size_t>(n));
        if (raw.size() > 65536)
            break;
    }
    ::close(fd);
    int status = 0;
    if (raw.size() >= 12 && raw.compare(0, 5, "HTTP/") == 0)
        status = std::atoi(raw.c_str() + 9);
    const auto hdr_end = raw.find("\r\n\r\n");
    if (body) {
        if (hdr_end != std::string::npos)
            *body = raw.substr(hdr_end + 4);
        else
            *body = raw;
    }
    return status;
}

int ParseHttpPort(const std::string &field) {
    if (field.empty() || field == "-")
        return 0;
    const auto colon = field.rfind(':');
    if (colon != std::string::npos)
        return std::atoi(field.c_str() + colon + 1);
    return std::atoi(field.c_str());
}

struct ClusterProc {
    std::string role, iid, rpc, comm, live_detail, ready_detail;
    int pid = 0;
    int http_port = 0;
    int game_port = 0;
    int64_t rss_kb = 0;
    int live_http = 0;
    int ready_http = 0;
    bool alive = false;
    bool live = false;
    bool ready = false;
};

void ProbeClusterProc(ClusterProc *p) {
    if (!p || !p->alive)
        return;
    if (p->pid == ::getpid()) {
        p->live = ServiceHealth::Instance().IsLive(30);
        const HealthDepsResult deps = EvaluateHealthDeps(ServiceHealth::Instance().service());
        p->ready = ServiceHealth::Instance().ready() && deps.ok;
        p->live_detail = "self";
        p->ready_detail = deps.detail;
        p->live_http = p->live ? 200 : 503;
        p->ready_http = p->ready ? 200 : 503;
        return;
    }
    if (p->http_port <= 0)
        return;
    std::string body;
    // ready 路径可能同步 Ping GameDB（约 800ms），超时过短会误报未就绪
    p->live_http = HttpGetLocal(p->http_port, "/health/live", 1500, &body);
    p->live = (p->live_http == 200);
    p->live_detail = body.size() > 240 ? body.substr(0, 240) : body;
    body.clear();
    p->ready_http = HttpGetLocal(p->http_port, "/health/ready", 1500, &body);
    p->ready = (p->ready_http == 200);
    p->ready_detail = body.size() > 240 ? body.substr(0, 240) : body;
}

Json::Value BuildClusterStatusJson() {
    Json::Value out;
    out["ok"] = true;
    const std::string path = InventoryTsvPath();
    out["inventory"] = path;
    std::vector<ClusterProc> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream iss(line);
        std::string role, iid, pid_s, rpc, http, game;
        if (!(iss >> role >> iid >> pid_s >> rpc >> http >> game))
            continue;
        ClusterProc p;
        p.role = role;
        p.iid = iid;
        p.pid = std::atoi(pid_s.c_str());
        p.rpc = rpc;
        p.http_port = ParseHttpPort(http);
        p.game_port = ParseHttpPort(game);
        p.alive = PidAlive(p.pid);
        p.comm = p.alive ? ProcComm(p.pid) : "";
        p.rss_kb = p.alive ? ProcRssKb(p.pid) : 0;
        rows.push_back(std::move(p));
    }
    std::vector<std::thread> workers;
    workers.reserve(rows.size());
    for (auto &p : rows)
        workers.emplace_back([&p]() { ProbeClusterProc(&p); });
    for (auto &t : workers)
        t.join();
    Json::Value procs(Json::arrayValue);
    int alive_n = 0, live_n = 0, ready_n = 0;
    for (const auto &r : rows) {
        Json::Value p;
        p["role"] = r.role;
        p["instance_id"] = r.iid;
        p["pid"] = r.pid;
        p["alive"] = r.alive;
        p["rpc_addr"] = r.rpc;
        p["http_port"] = r.http_port;
        p["game_port"] = r.game_port;
        p["comm"] = r.comm;
        p["rss_kb"] = static_cast<Json::Int64>(r.rss_kb);
        p["live"] = r.live;
        p["ready"] = r.ready;
        p["live_http"] = r.live_http;
        p["ready_http"] = r.ready_http;
        p["live_detail"] = r.live_detail;
        p["ready_detail"] = r.ready_detail;
        if (r.alive)
            ++alive_n;
        if (r.live)
            ++live_n;
        if (r.ready)
            ++ready_n;
        procs.append(p);
    }
    const int n = static_cast<int>(rows.size());
    out["process_n"] = n;
    out["alive_n"] = alive_n;
    out["live_n"] = live_n;
    out["ready_n"] = ready_n;
    out["expected_n"] = 8;
    out["all_alive"] = (n > 0 && alive_n == n);
    out["all_ready"] = (n > 0 && ready_n == n);
    out["processes"] = procs;
    if (n == 0) {
        out["ok"] = false;
        out["error"] = "inventory.tsv empty or missing";
    }
    return out;
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
        Json::Value j;
        auto *pool = ConnectionPool::getconnectionPool();
        j["ok"] = pool->isInitialized();
        j["dbname"] = pool->dbname();
        j["table"] = "gamemesh_metrics";
        j["flush_interval_sec"] = 10;
        if (pool->isInitialized()) {
            MetricsHistoryQuery q;
            if (MetricsDbWriter::Instance().QueryHistory(1, 1, &q)) {
                j["row_count"] = static_cast<Json::UInt64>(q.total_rows);
                if (q.last_ts_unix)
                    j["last_ts_unix"] = static_cast<Json::Int64>(q.last_ts_unix);
                if (!q.error.empty())
                    j["error"] = q.error;
            }
        }
        SendJson(response, j);
        return;
    }
    if (url == "/api/metrics/history") {
        int hours = 6;
        int limit = 500;
        const std::string hs = UrlParam(request, "hours");
        const std::string ls = UrlParam(request, "limit");
        if (!hs.empty())
            hours = std::atoi(hs.c_str());
        if (!ls.empty())
            limit = std::atoi(ls.c_str());
        MetricsHistoryQuery q;
        MetricsDbWriter::Instance().QueryHistory(hours, limit, &q);
        Json::Value j;
        j["ok"] = q.ok;
        j["mysql"] = q.mysql;
        j["table"] = "gamemesh_metrics";
        j["hours"] = hours;
        j["limit"] = limit;
        j["row_count"] = static_cast<Json::UInt64>(q.total_rows);
        j["last_ts_unix"] = static_cast<Json::Int64>(q.last_ts_unix);
        j["point_n"] = static_cast<Json::UInt>(q.points.size());
        if (!q.error.empty())
            j["error"] = q.error;
        if (!q.mysql) {
            j["hint"] = "Formal 下落库只在 GameDB 进程；请打开 GameDB HTTP（默认 8094）/monitor";
        }
        Json::Value arr(Json::arrayValue);
        for (const auto &p : q.points) {
            Json::Value e;
            e["t"] = static_cast<Json::Int64>(p.ts_unix);
            e["cpu_seconds_total"] = p.cpu_seconds_total;
            e["rss_bytes"] = static_cast<Json::Int64>(p.rss_bytes);
            e["rss_kb"] = static_cast<Json::Int64>(p.rss_bytes / 1024);
            e["vm_size_bytes"] = static_cast<Json::Int64>(p.vm_size_bytes);
            e["open_fds"] = p.open_fds;
            e["process_threads"] = p.process_threads;
            e["eventloop_tick_sec"] = p.eventloop_tick_sec;
            e["eventloop_tick_peak_sec"] = p.eventloop_tick_peak_sec;
            e["thread_states_json"] = p.thread_states_json;
            e["tcp_send_queue_bytes"] = static_cast<Json::UInt64>(p.tcp_send_queue_bytes);
            e["tcp_recv_queue_bytes"] = static_cast<Json::UInt64>(p.tcp_recv_queue_bytes);
            arr.append(e);
        }
        j["points"] = arr;
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
    // /health/live|/api/liveness|/api/health：进程 EventLoop 心跳存活
    if (url == "/health/live" || url == "/api/health" || url == "/api/liveness") {
        const bool live = ServiceHealth::Instance().IsLive(30);
        response->SetContentType("application/json");
        SendPlain(response, ServiceHealth::Instance().LivenessJson(),
                  live ? HttpResponse::HttpStatusCode::k200K
                       : HttpResponse::HttpStatusCode::k503ServiceUnavailable);
        return;
    }
    // /health/ready|/api/readiness：依赖 + 注册就绪；未 ready 不得进 LB
    if (url == "/health/ready" || url == "/api/readiness") {
        const HealthDepsResult deps = EvaluateHealthDeps(ServiceHealth::Instance().service());
        const bool ok = ServiceHealth::Instance().ready() && deps.ok;
        response->SetContentType("application/json");
        SendPlain(response, ServiceHealth::Instance().ReadinessJson(deps.ok, deps.detail),
                  ok ? HttpResponse::HttpStatusCode::k200K
                     : HttpResponse::HttpStatusCode::k503ServiceUnavailable);
        return;
    }
    if (url == "/api/version") {
        response->SetContentType("application/json");
        SendPlain(response, ServiceHealth::Instance().VersionJson());
        return;
    }
    if (url == "/api/cluster" || url == "/api/cluster/status") {
        SendJson(response, BuildClusterStatusJson());
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
#ifdef WEBSERVER_ENABLE_BRPC
    const int logic_port_override = opts.logic_port_override;
#endif
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
#ifdef WEBSERVER_ENABLE_MYSQL
    // 正式模式：仅 gamedb（及紧急 all）可建 MySQL 池；必须在首次 getconnectionPool 之前
    if (!FormalModeAllowsMysql(role)) {
        ::setenv("GAMEMESH_FORBID_MYSQL", "1", 1);
        ConnectionPool::ForbidInit(
            "formal mode: MySQL only on role=gamedb (use GameDB brpc)");
    }
#endif
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
#ifdef WEBSERVER_ENABLE_BRPC
    // 本进程已注册到 StaticServiceRegistry 的 (service, instance_id)，优雅退出时 DRAINING+注销
    struct LocalReg {
        std::string service;
        std::string instance_id;
        int ttl_sec = 30;
    };
    static std::vector<LocalReg> g_local_regs;
    static std::mutex g_local_regs_mu;
    auto track_reg = [](const std::string &service, const std::string &instance_id,
                        int ttl_sec = 30) {
        std::lock_guard<std::mutex> lk(g_local_regs_mu);
        g_local_regs.push_back({service, instance_id, ttl_sec});
    };
    auto mark_local_draining = []() {
        std::lock_guard<std::mutex> lk(g_local_regs_mu);
        for (const auto &r : g_local_regs) {
            StaticServiceRegistry::Get().SetInstanceStatus(r.service, r.instance_id, "DRAINING");
            if (RedisServiceRegistry::Get().ready())
                RedisServiceRegistry::Get().SetInstanceStatus(r.service, r.instance_id, "DRAINING");
        }
    };
    auto unregister_local = []() {
        std::lock_guard<std::mutex> lk(g_local_regs_mu);
        for (const auto &r : g_local_regs) {
            StaticServiceRegistry::Get().UnregisterInstance(r.service, r.instance_id);
            if (RedisServiceRegistry::Get().ready())
                RedisServiceRegistry::Get().UnregisterInstance(r.service, r.instance_id);
        }
        g_local_regs.clear();
    };
    auto resolve_instance_id = [](const std::string &prefix, int port,
                                  const char *fallback) -> std::string {
        if (const char *env = std::getenv("GAMEMESH_INSTANCE_ID")) {
            if (env[0])
                return env;
        }
        if (fallback && fallback[0])
            return fallback;
        return prefix + "-" + std::to_string(port > 0 ? port : 0);
    };
#endif
    std::signal(SIGTERM, [](int) {
        ServiceHealth::Instance().SetDraining(true);
#ifdef WEBSERVER_ENABLE_BRPC
        // 信号处理内仅置位；DRAINING/Unregister 在 loop 退出后的 graceful 路径完成
#endif
        if (g_main_loop)
            g_main_loop->Quit();
    });
    std::signal(SIGINT, [](int) {
        ServiceHealth::Instance().SetDraining(true);
        if (g_main_loop)
            g_main_loop->Quit();
    });
#ifdef WEBSERVER_ENABLE_MYSQL
    const bool need_db = FormalModeAllowsMysql(role) &&
                         (role == "all" || role == "gamelogic" || role == "world" || role == "gamedb");
    if (FormalModeEnabled() && !FormalModeAllowsMysql(role)) {
        LOG_INFO << "formal mode: skip MySQL pool/stores for role=" << role
                 << " (assets/mail via GameDB)";
    } else if (need_db && ConnectionPool::getconnectionPool()->isInitialized()) {
        if (role != "gamedb")
            MetricsDbWriter::Instance().StartPeriodic(&loop, 10.0);
        if (role != "gamedb" && role != "world") {
            PlayerItemStore::Instance().EnsureTable();
            PlayerAccountStore::Instance().EnsureTable();
            PlayerItemPersistQueue::Instance().StartPeriodic(&loop, 300.0);
        } else if (role == "world") {
            PlayerItemStore::Instance().EnsureTable();
        } else if (role == "gamedb") {
            PlayerAccountStore::Instance().EnsureTable();
            PlayerProfileStore::Instance().EnsureTable();
            PlayerItemStore::Instance().EnsureTable();
            GameDbAssetStore::Instance().EnsureTables();
            GameDbOutbox::Instance().EnsureTable();
        }
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
        // world：若配置 gamedb_addrs，先挂 BrpcGameDb 再 Init MailService（见下方 brpc 分支）
        if (role == "all" || role == "gamelogic" || role == "gamedb") {
            if (MailService::Instance().Init()) {
                if (role == "all" || role == "gamedb")
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
        role == "session" || role == "gamedb") {
        if (!SessionStore::Instance().InitFromConfig())
            LOG_WARN << "Redis session disabled (config/redis.cnf)";
        else {
            // Session 权威 Placement；Logic 也需可读 Redis Placement（EnterMap/lease）
            if (role == "all" || role == "session" || role == "gamelogic") {
                PlacementStore::Instance().InitFromSessionPrefix(
                    SessionStore::Instance().key_prefix());
            }
            // Gateway / GameLogic：跨 GW 可靠 Push 回放
            if (role == "all" || role == "gateway" || role == "gamelogic" || role == "session" ||
                role == "gamedb") {
                PushReplayStore::Instance().InitFromSessionPrefix(
                    SessionStore::Instance().key_prefix());
            }
#ifdef WEBSERVER_ENABLE_BRPC
            AuthLoginRateLimit::Instance().Configure(SessionStore::Instance().key_prefix());
            RedisServiceRegistry::Get().Configure(SessionStore::Instance().key_prefix());
            RedisServiceRegistry::Get().InitFromRedisConfig();
#endif
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

    auto apply_gateway_push_addrs = [&]() {
        std::string push_map = GatewayConfigPath::ReadValue("gateway_push_addrs");
        auto try_cnf = [&](const char *rel) {
            if (!push_map.empty())
                return;
            std::string p = std::string("../") + rel;
            std::string resolved;
            if (GameMeshPaths::ResolveProjectSubdir(rel, &resolved))
                p = resolved;
            push_map = load_kv(p, "gateway_push_addrs");
        };
        try_cnf("config/gamelogic.cnf");
        try_cnf("config/session.cnf");
        try_cnf("config/world.cnf");
        auto entries = split_addrs(push_map);
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
    };

    {
        const std::string etcd = GatewayConfigPath::ReadValue("etcd_endpoints");
        if (!etcd.empty())
            EtcdDiscovery::Instance().Configure(etcd);  // 默认 no-op；需 GAMEMESH_ENABLE_ETCD_V2=1
        else
            LOG_INFO << "etcd_endpoints empty; using static *_addrs + brpc list:// naming";
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
        const std::string listen = SessionBrpcServer::Instance().listen_addr();
        const int brpc_port = PortFromHostPort(listen);
        const std::string sid =
            resolve_instance_id("session", brpc_port > 0 ? brpc_port : logic_port_override, nullptr);
        const std::string adv = AdvertiseFromListen(listen);
        IServiceRegistry::ServiceInstance inst;
        inst.service = "session";
        inst.instance_id = sid;
        inst.address = adv;
        inst.port = brpc_port;
        inst.status = "UP";
        inst.protocol = "baidu_std";
        if (StaticServiceRegistry::Get().RegisterInstance(inst, 30)) {
            track_reg("session", sid, 30);
            LOG_INFO << "session registered id=" << sid << " advertise=" << adv;
        }
        if (RedisServiceRegistry::Get().ready())
            RedisServiceRegistry::Get().RegisterInstance(inst, 30);
        if (EtcdDiscovery::Instance().enabled())
            EtcdDiscovery::Instance().Register("session", sid, adv, 30);
        apply_gateway_push_addrs();
        LOG_INFO << "role=session listen=" << listen << " instance_id=" << sid;
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
        const std::string listen = GameDbBrpcServer::Instance().listen_addr();
        const int brpc_port = PortFromHostPort(listen);
        const char *fb =
            (logic_port_override == 8501 || logic_port_override <= 0) ? "gamedb-0" : "gamedb-1";
        const std::string gid = resolve_instance_id("gamedb", brpc_port, fb);
        const std::string adv = AdvertiseFromListen(listen);
        IServiceRegistry::ServiceInstance inst;
        inst.service = "gamedb";
        inst.instance_id = gid;
        inst.address = adv;
        inst.port = brpc_port;
        inst.status = "UP";
        if (StaticServiceRegistry::Get().RegisterInstance(inst, 30))
            track_reg("gamedb", gid, 30);
        if (RedisServiceRegistry::Get().ready())
            RedisServiceRegistry::Get().RegisterInstance(inst, 30);
        if (EtcdDiscovery::Instance().enabled())
            EtcdDiscovery::Instance().Register("gamedb", gid, adv, 30);
        {
            std::string session_addr = GatewayConfigPath::ReadValue("session_addrs");
            if (session_addr.empty()) {
                std::string logic_cnf = "../config/gamelogic.cnf";
                std::string resolved;
                if (GameMeshPaths::ResolveProjectSubdir("config/gamelogic.cnf", &resolved))
                    logic_cnf = resolved;
                session_addr = load_kv(logic_cnf, "session_addrs");
            }
            if (!session_addr.empty())
                SessionRpcClient::Instance().Init(session_addr);
        }
        apply_gateway_push_addrs();
        LOG_INFO << "role=gamedb listen=" << listen << " instance_id=" << gid
                 << " advertise=" << adv;
    } else if (role == "gamelogic") {
        std::string listen = "0.0.0.0:8201";
        if (logic_port_override > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "0.0.0.0:%d", logic_port_override);
            listen = buf;
        }
        if (logic_port_override > 0) {
            // 必须尊重 GAMEMESH_INSTANCE_ID（start_formal/e2e 高端口 ≠ 8201）
            const char *fb = (logic_port_override == 8201) ? "gl-0" : "gl-1";
            const std::string iid =
                resolve_instance_id("gl", logic_port_override, fb);
            MapInstanceRegistry::Instance().SetLocalInstanceId(iid);
            if (FormalModeEnabled())
                MapInstanceRegistry::Instance().SetRequireLease(true);
            LOG_INFO << "gamelogic local instance_id=" << iid
                     << " listen_override=" << logic_port_override
                     << " require_lease=" << MapInstanceRegistry::Instance().require_lease();
            GameLogicBrpcServer::Instance().LoadMapRuntimeFromConfig();
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
            if (!session_addr.empty())
                SessionRpcClient::Instance().Init(session_addr);
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
            apply_gateway_push_addrs();
            std::string logic_listen = GameLogicBrpcServer::Instance().listen_addr();
            const std::string lid = MapInstanceRegistry::Instance().local_instance_id();
            const std::string adv = AdvertiseFromListen(logic_listen);
            IServiceRegistry::ServiceInstance inst;
            inst.service = "gamelogic";
            inst.instance_id = lid.empty() ? resolve_instance_id("gl", PortFromHostPort(logic_listen),
                                                                "gl-0")
                                           : lid;
            inst.address = adv;
            inst.port = PortFromHostPort(logic_listen);
            inst.status = "UP";
            if (StaticServiceRegistry::Get().RegisterInstance(inst, 30))
                track_reg("gamelogic", inst.instance_id, 30);
            if (RedisServiceRegistry::Get().ready())
                RedisServiceRegistry::Get().RegisterInstance(inst, 30);
            if (EtcdDiscovery::Instance().enabled())
                EtcdDiscovery::Instance().Register("gamelogic", inst.instance_id, adv, 30);
            LOG_INFO << "gamelogic registered id=" << inst.instance_id << " advertise=" << adv;
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
        std::vector<std::string> session_addrs;
        std::vector<std::string> session_ids;
        {
            std::string session_csv = GatewayConfigPath::ReadValue("session_addrs");
            GatewayConfigPath::SplitCsv(session_csv, &session_addrs);
            std::string sid_csv = GatewayConfigPath::ReadValue("session_instance_ids");
            GatewayConfigPath::SplitCsv(sid_csv, &session_ids);
        }
        // 遗留 etcd v2：仅在显式开启时合并发现结果（不得截首地址）
        if (EtcdDiscovery::Instance().enabled()) {
            std::vector<std::string> discovered;
            if (EtcdDiscovery::Instance().Discover("gamelogic", &discovered) && !discovered.empty()) {
                addrs = discovered;
                LOG_WARN << "legacy etcd v2 gamelogic addrs=" << addrs.size();
            }
            if (EtcdDiscovery::Instance().Discover("world", &discovered) && !discovered.empty()) {
                world_addrs = discovered;
                LOG_WARN << "legacy etcd v2 world addrs=" << world_addrs.size();
            }
            if (EtcdDiscovery::Instance().Discover("session", &discovered) && !discovered.empty()) {
                session_addrs = discovered;
                LOG_WARN << "legacy etcd v2 session addrs=" << session_addrs.size();
            }
        }
        if (!BrpcTransport::Instance().EnsureStarted(addrs, ids, world_addrs, timeout_ms)) {
            LOG_ERROR << "BrpcTransport init failed";
            return 1;
        }
        if (!session_addrs.empty()) {
            SessionRpcClient::Instance().Init(session_addrs, timeout_ms);
            GatewayAuthClients::Instance().InitAuthSession(session_addrs, timeout_ms);
        } else {
            LOG_INFO << "session_addrs empty; gateway uses local SessionStore";
        }
        // Auth/Session 编排客户端 + Logic Bind/Dispatch Channel（复用，非逐请求创建）
        {
            GatewayAuthClients::Instance().InitLogicChannels(addrs, ids, timeout_ms);
            StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", addrs, ids);
            StaticServiceRegistry::Get().SetStaticAddrs("session", session_addrs, session_ids);
            StaticServiceRegistry::Get().SetStaticAddrs("world", world_addrs);
        }
        // 统一 GatewayInstanceId（GameTCP / Push / Session 绑定共用）
        {
            std::string id_err;
            if (!GatewayIdentity::Instance().Resolve(&id_err)) {
                LOG_ERROR << "GatewayIdentity resolve failed: " << id_err;
                return 1;
            }
            if (!GatewayIdentity::Instance().ClaimOrFail(&id_err)) {
                LOG_ERROR << "GatewayIdentity claim failed: " << id_err;
                return 1;
            }
        }
        const std::string &gw_id = GatewayIdentity::Instance().id();
        // GatewayPush 内网口：listen 与 advertise 分离；身份用 gw_id 而非端口拼接
        {
            const int push_port = (game_port > 0 ? game_port : port) + 100;
            char listen_buf[64];
            snprintf(listen_buf, sizeof(listen_buf), "0.0.0.0:%d", push_port);
            GatewayPushServer::Instance().set_gateway_instance_id(gw_id);
            if (!GatewayPushServer::Instance().Start(listen_buf))
                LOG_WARN << "GatewayPushServer start failed " << listen_buf;
            const std::string adv = MakeAdvertiseAddr(push_port);
            if (EtcdDiscovery::Instance().enabled())
                EtcdDiscovery::Instance().Register("gateway_push", gw_id, adv, 30);
            IServiceRegistry::ServiceInstance inst;
            inst.service = "gateway_push";
            inst.instance_id = gw_id;
            inst.address = adv;
            inst.port = push_port;
            inst.status = "UP";
            if (StaticServiceRegistry::Get().RegisterInstance(inst, 30))
                track_reg("gateway_push", gw_id, 30);
            if (RedisServiceRegistry::Get().ready())
                RedisServiceRegistry::Get().RegisterInstance(inst, 30);
            GatewayPushClient::Instance().SetGatewayPushAddr(gw_id, adv);
        }
        {
            const int gp = game_port > 0 ? game_port : port;
            const std::string adv = MakeAdvertiseAddr(gp);
            IServiceRegistry::ServiceInstance inst;
            inst.service = "gateway";
            inst.instance_id = gw_id;
            inst.address = adv;
            inst.port = gp;
            inst.status = "UP";
            if (StaticServiceRegistry::Get().RegisterInstance(inst, 30))
                track_reg("gateway", gw_id, 30);
            if (RedisServiceRegistry::Get().ready())
                RedisServiceRegistry::Get().RegisterInstance(inst, 30);
            if (EtcdDiscovery::Instance().enabled())
                EtcdDiscovery::Instance().Register("gateway", gw_id, adv, 30);
        }
        // 进程内 lease 续租 + 可选 cnf 热加载（无 etcd v3 时的动态发现降级）
        {
            const char *poll_env = std::getenv("GAMEMESH_DISCOVERY_POLL_SEC");
            double poll_sec = poll_env && *poll_env ? std::atof(poll_env) : 5.0;
            if (poll_sec < 1.0)
                poll_sec = 5.0;
            loop.RunEvery(poll_sec, []() {
                std::lock_guard<std::mutex> lk(g_local_regs_mu);
                for (const auto &r : g_local_regs) {
                    StaticServiceRegistry::Get().RenewInstance(r.service, r.instance_id, r.ttl_sec);
                    if (RedisServiceRegistry::Get().ready())
                        RedisServiceRegistry::Get().RenewInstance(r.service, r.instance_id,
                                                                 r.ttl_sec);
                }
            });
            loop.RunEvery(poll_sec, [timeout_ms]() {
                static std::string last_logic;
                static std::string last_sess;
                static std::string last_gamedb;
                std::vector<std::string> logic_addrs, logic_ids, world_addrs, sess_addrs, sess_ids;
                int to = timeout_ms;
                // Redis 动态发现优先；失败再回落 cnf（空发现不覆盖健康 Channel）
                std::vector<IServiceRegistry::ServiceInstance> redis_logic;
                if (RedisServiceRegistry::Get().ready() &&
                    RedisServiceRegistry::Get().Discover("gamelogic", &redis_logic) &&
                    !redis_logic.empty()) {
                    logic_addrs.clear();
                    logic_ids.clear();
                    for (const auto &i : redis_logic) {
                        logic_addrs.push_back(i.address);
                        logic_ids.push_back(i.instance_id);
                    }
                } else if (!GatewayConfigPath::Load(&logic_addrs, &logic_ids, &world_addrs, &to)) {
                    return;
                }
                std::string logic_key;
                for (size_t i = 0; i < logic_addrs.size(); ++i) {
                    if (i)
                        logic_key.push_back('|');
                    logic_key += (i < logic_ids.size() ? logic_ids[i] : "") + "=" + logic_addrs[i];
                }
                if (!logic_addrs.empty() && logic_key != last_logic) {
                    StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", logic_addrs, logic_ids);
                    BrpcChannelManager::Instance().ApplySnapshot(logic_addrs, logic_ids);
                    GatewayAuthClients::Instance().InitLogicChannels(logic_addrs, logic_ids, to);
                    last_logic = logic_key;
                    LOG_INFO << "discovery ApplySnapshot gamelogic n=" << logic_addrs.size();
                }
                std::vector<IServiceRegistry::ServiceInstance> redis_sess;
                if (RedisServiceRegistry::Get().ready() &&
                    RedisServiceRegistry::Get().Discover("session", &redis_sess) &&
                    !redis_sess.empty()) {
                    sess_addrs.clear();
                    sess_ids.clear();
                    for (const auto &i : redis_sess) {
                        sess_addrs.push_back(i.address);
                        sess_ids.push_back(i.instance_id);
                    }
                }
                // 与静态 cnf 求并集：避免 Redis 注册滞后时把双 Session 收成单点
                {
                    std::vector<std::string> static_addrs, static_ids;
                    GatewayConfigPath::SplitCsv(GatewayConfigPath::ReadValue("session_addrs"),
                                                &static_addrs);
                    GatewayConfigPath::SplitCsv(GatewayConfigPath::ReadValue("session_instance_ids"),
                                                &static_ids);
                    for (size_t i = 0; i < static_addrs.size(); ++i) {
                        bool found = false;
                        for (const auto &a : sess_addrs) {
                            if (a == static_addrs[i]) {
                                found = true;
                                break;
                            }
                        }
                        if (!found && !static_addrs[i].empty()) {
                            sess_addrs.push_back(static_addrs[i]);
                            if (i < static_ids.size())
                                sess_ids.push_back(static_ids[i]);
                        }
                    }
                    if (sess_addrs.empty()) {
                        sess_addrs = std::move(static_addrs);
                        sess_ids = std::move(static_ids);
                    }
                }
                std::string sess_key;
                for (size_t i = 0; i < sess_addrs.size(); ++i) {
                    if (i)
                        sess_key.push_back('|');
                    sess_key += (i < sess_ids.size() ? sess_ids[i] : "") + "=" + sess_addrs[i];
                }
                if (!sess_addrs.empty() && sess_key != last_sess) {
                    StaticServiceRegistry::Get().SetStaticAddrs("session", sess_addrs, sess_ids);
                    SessionRpcClient::Instance().Init(sess_addrs, to);
                    GatewayAuthClients::Instance().InitAuthSession(sess_addrs, to);
                    last_sess = sess_key;
                }
                std::vector<std::string> gdb_addrs;
                std::vector<IServiceRegistry::ServiceInstance> redis_gdb;
                if (RedisServiceRegistry::Get().ready() &&
                    RedisServiceRegistry::Get().Discover("gamedb", &redis_gdb) &&
                    !redis_gdb.empty()) {
                    for (const auto &i : redis_gdb)
                        gdb_addrs.push_back(i.address);
                } else {
                    std::string gc = GatewayConfigPath::ReadValue("gamedb_addrs");
                    GatewayConfigPath::SplitCsv(gc, &gdb_addrs);
                }
                std::string gdb_key;
                for (size_t i = 0; i < gdb_addrs.size(); ++i) {
                    if (i)
                        gdb_key.push_back('|');
                    gdb_key += gdb_addrs[i];
                }
                if (!gdb_addrs.empty() && gdb_key != last_gamedb) {
                    BrpcGameDbRepository::Instance().ApplySnapshot(gdb_addrs);
                    last_gamedb = gdb_key;
                }
            });
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
        apply_gateway_push_addrs();
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
        const std::string listen = WorldBrpcServer::Instance().listen_addr();
        const std::string wid =
            resolve_instance_id("world", PortFromHostPort(listen), "world-1");
        const std::string adv = AdvertiseFromListen(listen);
        IServiceRegistry::ServiceInstance inst;
        inst.service = "world";
        inst.instance_id = wid;
        inst.address = adv;
        inst.port = PortFromHostPort(listen);
        inst.status = "UP";
        if (StaticServiceRegistry::Get().RegisterInstance(inst, 30))
            track_reg("world", wid, 30);
        if (EtcdDiscovery::Instance().enabled())
            EtcdDiscovery::Instance().Register("world", wid, adv, 30);
        LOG_INFO << "role=world listen=" << listen << " instance_id=" << wid
                 << " advertise=" << adv;
    }

    // 非 gateway 角色：进程内注册续租 + Redis 发现续租
    if (role == "session" || role == "gamelogic" || role == "world" || role == "gamedb") {
        loop.RunEvery(5.0, []() {
            std::lock_guard<std::mutex> lk(g_local_regs_mu);
            for (const auto &r : g_local_regs) {
                StaticServiceRegistry::Get().RenewInstance(r.service, r.instance_id, r.ttl_sec);
                if (RedisServiceRegistry::Get().ready())
                    RedisServiceRegistry::Get().RenewInstance(r.service, r.instance_id, r.ttl_sec);
            }
        });
    }
#ifdef WEBSERVER_ENABLE_REDIS
    // Session / Gateway：动态刷新健康 GameLogic Owner 列表
    if (role == "session" || role == "gateway" || role == "all") {
        if (RedisServiceRegistry::Get().ready())
            RefreshHealthyLogicOwners(true);
        loop.RunEvery(5.0, []() {
            if (!RedisServiceRegistry::Get().ready())
                return;
            RefreshHealthyLogicOwners(true);
        });
    }
#endif
    // GameLogic / World / GameDB / Session：Push 与 KickConnection 都走 gateway_push
    if (role == "gamelogic" || role == "world" || role == "gamedb" || role == "session" ||
        role == "all") {
        loop.RunEvery(5.0, []() {
            if (!RedisServiceRegistry::Get().ready())
                return;
            std::vector<IServiceRegistry::ServiceInstance> insts;
            if (!RedisServiceRegistry::Get().Discover("gateway_push", &insts) || insts.empty())
                return;
            for (const auto &i : insts) {
                if (!i.instance_id.empty() && !i.address.empty())
                    GatewayPushClient::Instance().SetGatewayPushAddr(i.instance_id, i.address);
            }
        });
    }
#endif
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    // GameLogic：Owner lease 续租（默认 lease/3，至少 5s）
    if (role == "gamelogic" || role == "all") {
        uint32_t lease_sec = 30;
#ifdef WEBSERVER_ENABLE_REDIS
        if (PlacementStore::Instance().Available())
            lease_sec = static_cast<uint32_t>(PlacementStore::Instance().default_lease_sec());
#endif
        MapLeaseKeeper::Instance().SetLeaseSec(lease_sec);
        const double hb = std::max(5.0, static_cast<double>(lease_sec) / 3.0);
        loop.RunEvery(hb, []() { MapLeaseKeeper::Instance().Tick(); });
        LOG_INFO << "MapLeaseKeeper interval_sec=" << hb << " lease_sec=" << lease_sec;
    }
#ifdef WEBSERVER_ENABLE_REDIS
    // Session：Placement 自动恢复（experimental；正式稳定版默认关闭）
    if ((role == "session" || role == "all") &&
        ExperimentalFeatureEnabled("GAMEMESH_EXPERIMENTAL_PLACEMENT_RECOVERY")) {
        if (const char *env = std::getenv("GAMEMESH_INSTANCE_ID"))
            PlacementRecoveryScheduler::Instance().SetInstanceId(env);
        if (const char *pfx = std::getenv("GAMEMESH_REDIS_PREFIX"))
            PlacementRecoveryScheduler::Instance().SetKeyPrefix(pfx);
        if (const char *sc = std::getenv("GAMEMESH_PLACEMENT_SCAN_COUNT")) {
            const int n = std::atoi(sc);
            if (n > 0)
                PlacementRecoveryScheduler::Instance().SetScanCount(static_cast<size_t>(n));
        }
        const double recover_iv =
            (std::getenv("GAMEMESH_PLACEMENT_RECOVER_IV") != nullptr)
                ? std::atof(std::getenv("GAMEMESH_PLACEMENT_RECOVER_IV"))
                : 5.0;
        const double iv = recover_iv > 0.5 ? recover_iv : 5.0;
        loop.RunEvery(iv, []() { PlacementRecoveryScheduler::Instance().Tick(); });
        LOG_INFO << "PlacementRecoveryScheduler (experimental) interval_sec=" << iv;
    }
#endif
#endif

#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    GameTcpGateway *game_tcp_gw = nullptr;
    if (role == "all" || role == "gateway") {
        if (role == "all") {
            InProcessTransport::Instance().EnsureStarted(0);
            MapInstanceRegistry::Instance().SetLocalInstanceId("gl-local");
            MapPlacement::Instance().ConfigureOwners({"gl-local"});
        }
        {
            std::string id_err;
            if (!GatewayIdentity::Instance().ready() &&
                !GatewayIdentity::Instance().Resolve(&id_err)) {
                LOG_ERROR << "GatewayIdentity resolve failed: " << id_err;
                return 1;
            }
        }
        {
            std::string map_err;
            if (!MapCatalog::Instance().EnsureDefault(&map_err)) {
                LOG_ERROR << "MapCatalog load failed: " << map_err;
                if (FormalModeEnabled())
                    return 1;
            }
        }
        game_tcp_gw = new GameTcpGateway("0.0.0.0", game_port, GatewayIdentity::Instance().id());
        game_tcp_gw->StartInBackground();
        LOG_INFO << "Game protobuf TCP " << game_port << " (HTTP " << port << ") role=" << role
                 << " gateway_instance_id=" << game_tcp_gw->gateway_instance_id();
    }
#endif

#ifdef WEBSERVER_ENABLE_BRPC
    if (role == "all") {
        if (!BrpcGameServer::Instance().StartFromConfig())
            LOG_WARN << "brpc MailBrpcService disabled (config/brpc.cnf or bind failed)";
    }
#endif
    // EventLoop 心跳：刷新 /health/live
    loop.RunEvery(1.0, []() { ServiceHealth::Instance().MarkAlive(); });
    loop.RunEvery(2.0, [role]() { HealthProbeStore::Instance().Refresh(role); });
    HealthProbeStore::Instance().Refresh(role);
#ifdef WEBSERVER_ENABLE_MYSQL
    if (role == "gamedb" || role == "all") {
        loop.RunEvery(5.0, []() {
            OpsMetrics::Instance().SetOutboxBacklog(
                static_cast<int64_t>(GameDbOutbox::Instance().CountUnpublished()));
        });
    }
#endif
    ServiceHealth::Instance().SetReady(true);
    ServiceHealth::Instance().MarkAlive();
    server.start();
    // ---- SIGTERM / Quit 之后：摘流 → 等在途 → 注销 → 停 brpc ----
    ServiceHealth::Instance().SetDraining(true);
    ServiceHealth::Instance().SetReady(false);
    LOG_INFO << "graceful shutdown begin role=" << role;
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    if (game_tcp_gw)
        game_tcp_gw->StopAccepting();
#endif
#ifdef WEBSERVER_ENABLE_BRPC
    mark_local_draining();
#endif
    {
        int drain_sec = 2;
        if (const char *e = std::getenv("GAMEMESH_DRAIN_SEC")) {
            const int v = std::atoi(e);
            if (v >= 0 && v <= 60)
                drain_sec = v;
        }
        if (drain_sec > 0) {
            LOG_INFO << "drain wait_sec=" << drain_sec << " (reject new login/enter_map)";
            std::this_thread::sleep_for(std::chrono::seconds(drain_sec));
        }
    }
#ifdef WEBSERVER_ENABLE_BRPC
    unregister_local();
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    if (role == "gamelogic" || role == "all")
        GameLogic::Instance().FlushAllLastSafe("graceful_shutdown");
#endif
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
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    if (game_tcp_gw) {
        game_tcp_gw->RequestQuit();
        delete game_tcp_gw;
        game_tcp_gw = nullptr;
    }
#endif
    LOG_INFO << "graceful shutdown done role=" << role;
    return 0;
}
