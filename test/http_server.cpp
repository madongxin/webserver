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
#include "WebServerPaths.h"

#ifdef WEBSERVER_ENABLE_MYSQL
#include "ConnectionPool.h"
#include "MetricsDbWriter.h"
#include "PlayerItemPersistQueue.h"
#include "PlayerItemStore.h"
#endif
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#include "GameTcpGateway.h"
#endif
#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
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
    FindAllFiles(WebServerPaths::FilesRoot(), &names);
    Json::Value arr(Json::arrayValue);
    for (const auto &n : names)
        arr.append(n);
    return arr;
}

std::string BuildFileHtml() {
    std::ostringstream os;
    os << "<html><body><h3>Files</h3><ul>";
    std::vector<std::string> names;
    FindAllFiles(WebServerPaths::FilesRoot(), &names);
    for (const auto &n : names)
        os << "<li><a href=\"/download?file=" << n << "\">" << n << "</a></li>";
    os << "</ul></body></html>";
    return os.str();
}

bool DownloadFile(const std::string &name, HttpResponse *response, bool head_only) {
    const std::string path = WebServerPaths::FilesRoot() + "/" + name;
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
        const std::string user = UrlParam(request, "username");
        const std::string pass = UrlParam(request, "password");
        if (user == "demo" && pass == "demo") {
            const std::string sid = NewGmSessionId();
            response->AddHeader("Set-Cookie", "gm_sid=" + sid + "; Path=/");
            SendHtml(response, "<html><body>login ok <a href=\"/gm\">GM</a></body></html>", head_only);
        } else if (user == "asan" && pass == "asan") {
            crash_uaf();
            SendPlain(response, "never");
        } else {
            SendPlain(response, "login failed", HttpResponse::HttpStatusCode::k403Forbidden);
        }
        return;
    }

    if (url == "/gm") {
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
        j["table"] = "webserver_metrics";
        j["flush_interval_sec"] = 10;
        if (pool->isInitialized()) {
            if (auto conn = pool->getConnection()) {
                MYSQL_RES *res =
                    conn->query("SELECT COUNT(*), MAX(ts_unix) FROM webserver_metrics");
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
#endif
    if (url == "/api/health") {
        Json::Value j;
        j["status"] = "ok";
        SendJson(response, j);
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
        const std::string path = WebServerPaths::FilesRoot() + "/" + file;
        const int rc = ::remove(path.c_str());
        Json::Value j;
        j["removed"] = (rc == 0);
        SendJson(response, j);
        return;
    }

    if (url == "/" || url == "/index.html") {
        std::string body = ReadFile(WebServerPaths::StaticRoot() + "/index.html");
        if (body.empty())
            body = "<html><body><h3>CppWebServer</h3><ul>"
                   "<li><a href=\"/monitor\">monitor</a></li>"
                   "<li><a href=\"/metrics\">metrics</a></li>"
                   "<li><a href=\"/fileserver\">fileserver</a></li>"
                   "<li><a href=\"/gm\">gm</a></li></ul></body></html>";
        SendHtml(response, body, head_only);
        return;
    }
    if (url == "/monitor" || url == "/monitor.html") {
        std::string body = ReadFile(WebServerPaths::StaticRoot() + "/monitor.html");
        if (body.empty())
            body = "<html><body>缺少 static/monitor.html</body></html>";
        SendHtml(response, body, head_only);
        return;
    }
    if (url == "/fileserver" || url == "/fileserver.html") {
        std::string body = ReadFile(WebServerPaths::StaticRoot() + "/fileserver.html");
        if (body.empty())
            body = BuildFileHtml();
        SendHtml(response, body, head_only);
        return;
    }
    if (url == "/mhw" || url == "/mhw.html") {
        SendHtml(response, ReadFile(WebServerPaths::StaticRoot() + "/mhw.html"), head_only);
        return;
    }
    if (url == "/about") {
        SendHtml(response, "<html><body><h3>CppWebServer</h3></body></html>", head_only);
        return;
    }

    if (url.size() > 1 && url[0] == '/') {
        std::string path = WebServerPaths::StaticRoot() + url;
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

int main(int argc, char *argv[]) {
    Logger::setLogLevel(Logger::INFO);
    ::mkdir("./log", 0755);
    asynclog = new AsyncLogging("./log/webserver.log");
    asynclog->Start();
    Logger::setOutput(AsyncOutputFunc);
    Logger::setFlush(AsyncFlushFunc);

    int port = 8080;
    int game_port = 0;
    if (argc <= 1) {
        game_port = port + 1;
    } else if (argc == 2) {
        port = std::atoi(argv[1]);
        game_port = port + 1;
    } else if (argc == 3) {
        port = std::atoi(argv[1]);
        game_port = std::atoi(argv[2]);
    } else {
        printf("usage: webserver [http_port] [game_protobuf_port]\n");
        return 0;
    }
    g_game_port = game_port;

    EventLoop loop;
#ifdef WEBSERVER_ENABLE_MYSQL
    if (ConnectionPool::getconnectionPool()->isInitialized()) {
        MetricsDbWriter::Instance().StartPeriodic(&loop, 10.0);
        PlayerItemStore::Instance().EnsureTable();
        // 道具落库：在线玩家每 300s 批量刷盘；登出时由 GameLogic 立即 FlushPlayer
        PlayerItemPersistQueue::Instance().StartPeriodic(&loop, 300.0);
    } else {
        LOG_WARN << "MySQL pool not initialized (config/mysql.cnf)";
    }
#endif
#ifdef WEBSERVER_ENABLE_REDIS
    if (!SessionStore::Instance().InitFromConfig())
        LOG_WARN << "Redis session disabled (config/redis.cnf)";
#endif
    ResourceWatchdog::Instance().StartPeriodic(&loop, 5.0);

    const unsigned ncpu = std::thread::hardware_concurrency();
    const int size = ncpu > 0 ? static_cast<int>(ncpu) : 4;
    HttpServer server(&loop, "0.0.0.0", port, true);
    server.SetHttpCallback(HttpResponseCallback);
    server.SetThreadNums(size);
#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
    {
        // 游戏端口独立线程：TcpServer -> OnMessage -> 拆帧 -> GameService -> GameLogic
        auto *gw = new GameTcpGateway("0.0.0.0", game_port);
        gw->StartInBackground();
        LOG_INFO << "Game protobuf TCP " << game_port << " (HTTP " << port << ")";
    }
#endif
    server.start();
    return 0;
}
