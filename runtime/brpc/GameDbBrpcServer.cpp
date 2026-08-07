#include "GameDbBrpcServer.h"

#include "AsyncMysqlGameDbRepository.h"
#include "GameDbServiceImpl.h"
#include "Logging.h"
#include "BrpcSslUtil.h"
#include "GameMeshPaths.h"

#include <brpc/server.h>

#include <cstdlib>
#include <fstream>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool ParseGameDbConfig(const std::string &path, std::string *listen_addr, int *idle,
                       std::string *nats_url) {
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
        else if (key == "nats_url" && nats_url)
            *nats_url = val;
    }
    return !listen_addr->empty();
}

// ssl_* 由 BrpcSslUtil::LoadFromCnf 读取

const std::string &GameDbCnf() {
    static std::string path = "../config/gamedb.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/gamedb.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace

GameDbBrpcServer &GameDbBrpcServer::Instance() {
    static GameDbBrpcServer g;
    return g;
}

GameDbBrpcServer::~GameDbBrpcServer() { Stop(); }

bool GameDbBrpcServer::StartFromConfig() {
    std::string addr = "0.0.0.0:8501";
    int idle = 30;
    std::string nats;
    if (!ParseGameDbConfig(GameDbCnf(), &addr, &idle, &nats))
        LOG_WARN << "GameDbBrpcServer: default listen, cannot parse " << GameDbCnf();
    if (!nats.empty())
        AsyncMysqlGameDbRepository::Instance().SetNatsUrl(nats);
    if (!AsyncMysqlGameDbRepository::Instance().started())
        AsyncMysqlGameDbRepository::Instance().Start(2);
    return Start(addr, idle);
}

bool GameDbBrpcServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;
    if (!AsyncMysqlGameDbRepository::Instance().started())
        AsyncMysqlGameDbRepository::Instance().Start(2);
    server_.reset(new brpc::Server());
    service_.reset(new GameDbServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GameDbBrpcServer: AddService failed";
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(GameDbCnf(), &ssl);
    if (BrpcSslUtil::ApplyServer(&options, ssl))
        LOG_INFO << "GameDbBrpcServer SSL enabled cert=" << ssl.cert;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "GameDbBrpcServer: Start failed addr=" << listen_addr;
        return false;
    }
    listen_addr_ = listen_addr;
    running_ = true;
    LOG_INFO << "GameDbBrpcServer listening on " << listen_addr_;
    return true;
}

void GameDbBrpcServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    running_ = false;
}
