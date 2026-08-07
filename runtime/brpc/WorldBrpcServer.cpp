#include "WorldBrpcServer.h"

#include "Logging.h"
#include "BrpcSslUtil.h"
#include "PlayerSerialQueue.h"
#include "GameMeshPaths.h"
#include "WorldForwardServiceImpl.h"

#include <brpc/server.h>

#include <cstdlib>
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

bool ParseWorldConfig(const std::string &path, std::string *listen_addr, int *idle_timeout_sec) {
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
    }
    return !listen_addr->empty();
}

const std::string &WorldCnfPath() {
    static std::string path = "../config/world.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/world.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace

WorldBrpcServer &WorldBrpcServer::Instance() {
    static WorldBrpcServer g;
    return g;
}

WorldBrpcServer::~WorldBrpcServer() { Stop(); }

bool WorldBrpcServer::StartFromConfig() {
    std::string addr = "0.0.0.0:8301";
    int idle = 30;
    if (!ParseWorldConfig(WorldCnfPath(), &addr, &idle))
        LOG_WARN << "WorldBrpcServer: use default listen, cannot parse " << WorldCnfPath();
    return Start(addr, idle);
}

bool WorldBrpcServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;

    PlayerSerialQueue::Instance().Start(0);

    server_.reset(new brpc::Server());
    service_.reset(new WorldForwardServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "WorldBrpcServer: AddService failed";
        server_.reset();
        service_.reset();
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(WorldCnfPath(), &ssl);
    if (BrpcSslUtil::ApplyServer(&options, ssl))
        LOG_INFO << "WorldBrpcServer SSL enabled cert=" << ssl.cert;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "WorldBrpcServer: Start failed addr=" << listen_addr;
        server_.reset();
        service_.reset();
        return false;
    }
    listen_addr_ = listen_addr;
    running_ = true;
    LOG_INFO << "WorldBrpcServer listening on " << listen_addr_
             << " (WorldForward, no combat tick)";
    return true;
}

void WorldBrpcServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    running_ = false;
    LOG_INFO << "WorldBrpcServer stopped";
}
