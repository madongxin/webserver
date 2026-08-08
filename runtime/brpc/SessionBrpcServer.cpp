#include "SessionBrpcServer.h"

#include "AuthServiceImpl.h"
#include "AuthTokenStore.h"
#include "BrpcSslUtil.h"
#include "GameMeshPaths.h"
#include "Logging.h"
#include "PlacementStore.h"
#include "SessionServiceImpl.h"
#include "SessionStore.h"

#include <brpc/server.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
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

std::vector<std::string> SplitCsv(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = Trim(item);
        if (!item.empty())
            out.push_back(item);
    }
    return out;
}

bool ParseSessionConfig(const std::string &path, std::string *listen_addr, int *idle,
                        std::vector<std::string> *logic_ids) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "listen_addr")
            *listen_addr = val;
        else if (key == "idle_timeout_sec")
            *idle = std::atoi(val.c_str());
        else if (key == "logic_instance_ids" && logic_ids)
            *logic_ids = SplitCsv(val);
    }
    return !listen_addr->empty();
}

const std::string &SessionCnf() {
    static std::string path = "../config/session.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/session.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace

SessionBrpcServer &SessionBrpcServer::Instance() {
    static SessionBrpcServer g;
    return g;
}

SessionBrpcServer::~SessionBrpcServer() { Stop(); }

bool SessionBrpcServer::StartFromConfig() {
    std::string addr = "0.0.0.0:8401";
    int idle = 30;
    std::vector<std::string> logic_ids;
    if (!ParseSessionConfig(SessionCnf(), &addr, &idle, &logic_ids))
        LOG_WARN << "SessionBrpcServer: default listen, cannot parse " << SessionCnf();
    if (!logic_ids.empty()) {
        PlacementStore::Instance().SetLogicOwners(logic_ids);
        SessionStore::Instance().SetLogicInstanceIds(std::move(logic_ids));
    }
    AuthTokenStore::Instance().InitFromConfig();
    if (SessionStore::Instance().Available())
        PlacementStore::Instance().InitFromSessionPrefix(SessionStore::Instance().key_prefix());
    return Start(addr, idle);
}

bool SessionBrpcServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;
    // 端口覆盖启动时也加载 logic owners / Placement
    {
        std::vector<std::string> logic_ids;
        std::string addr_unused;
        int idle_unused = 30;
        if (ParseSessionConfig(SessionCnf(), &addr_unused, &idle_unused, &logic_ids) &&
            !logic_ids.empty()) {
            PlacementStore::Instance().SetLogicOwners(logic_ids);
            SessionStore::Instance().SetLogicInstanceIds(logic_ids);
        }
        if (SessionStore::Instance().Available())
            PlacementStore::Instance().InitFromSessionPrefix(SessionStore::Instance().key_prefix());
    }
    server_.reset(new brpc::Server());
    service_.reset(new SessionServiceImpl());
    auth_service_.reset(new AuthServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "SessionBrpcServer: AddService Session failed";
        return false;
    }
    if (server_->AddService(auth_service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "SessionBrpcServer: AddService Auth failed";
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(SessionCnf(), &ssl);
    if (BrpcSslUtil::ApplyServer(&options, ssl))
        LOG_INFO << "SessionBrpcServer SSL enabled cert=" << ssl.cert;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "SessionBrpcServer: Start failed addr=" << listen_addr;
        return false;
    }
    listen_addr_ = listen_addr;
    running_ = true;
    LOG_INFO << "SessionBrpcServer(+Auth) listening on " << listen_addr_;
    return true;
}

void SessionBrpcServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    auth_service_.reset();
    running_ = false;
}
