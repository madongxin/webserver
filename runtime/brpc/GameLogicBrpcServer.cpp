#include "GameLogicBrpcServer.h"

#include "BrpcSslUtil.h"
#include "GameLogicForwardServiceImpl.h"
#include "GameLogicServiceImpl.h"
#include "GameMeshPaths.h"
#include "Logging.h"
#include "MapInstanceRegistry.h"
#include "PlayerSerialQueue.h"

#include <brpc/server.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool ParseLogicConfig(const std::string &path, std::string *listen_addr, int *idle_timeout_sec,
                      std::string *instance_id) {
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
            *idle_timeout_sec = std::atoi(val.c_str());
        else if (key == "instance_id" && instance_id)
            *instance_id = val;
    }
    return !listen_addr->empty();
}

const std::string &LogicCnfPath() {
    static std::string path = "../config/gamelogic.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/gamelogic.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace

GameLogicBrpcServer &GameLogicBrpcServer::Instance() {
    static GameLogicBrpcServer g;
    return g;
}

GameLogicBrpcServer::~GameLogicBrpcServer() {
    Stop();
}

bool GameLogicBrpcServer::StartFromConfig() {
    std::string addr = "0.0.0.0:8201";
    int idle = 30;
    std::string instance_id = "gl-0";
    if (!ParseLogicConfig(LogicCnfPath(), &addr, &idle, &instance_id))
        LOG_WARN << "GameLogicBrpcServer: use default listen, cannot parse " << LogicCnfPath();
    MapInstanceRegistry::Instance().SetLocalInstanceId(instance_id);
#ifdef WEBSERVER_ENABLE_BRPC
    // FormalMode.h 轻量；避免循环依赖时仅 env
#endif
    {
        const char *v = std::getenv("GAMEMESH_FORMAL");
        if (v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0))
            MapInstanceRegistry::Instance().SetRequireLease(true);
    }
    LOG_INFO << "GameLogicBrpcServer instance_id=" << instance_id
             << " require_lease=" << MapInstanceRegistry::Instance().require_lease();
    return Start(addr, idle);
}

bool GameLogicBrpcServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;

    PlayerSerialQueue::Instance().Start(0);

    server_.reset(new brpc::Server());
    service_.reset(new GameLogicForwardServiceImpl());
    gl_service_.reset(new GameLogicServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GameLogicBrpcServer: AddService Forward failed";
        server_.reset();
        service_.reset();
        gl_service_.reset();
        return false;
    }
    if (server_->AddService(gl_service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GameLogicBrpcServer: AddService GameLogicService failed";
        server_.reset();
        service_.reset();
        gl_service_.reset();
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(LogicCnfPath(), &ssl);
    if (BrpcSslUtil::ApplyServer(&options, ssl))
        LOG_INFO << "GameLogicBrpcServer SSL enabled cert=" << ssl.cert;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "GameLogicBrpcServer: Start failed addr=" << listen_addr;
        server_.reset();
        service_.reset();
        gl_service_.reset();
        return false;
    }
    listen_addr_ = listen_addr;
    running_ = true;
    LOG_INFO << "GameLogicBrpcServer listening on " << listen_addr_
             << " (GameLogicForward + GameLogicService)";
    return true;
}

void GameLogicBrpcServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    gl_service_.reset();
    running_ = false;
    LOG_INFO << "GameLogicBrpcServer stopped";
}
