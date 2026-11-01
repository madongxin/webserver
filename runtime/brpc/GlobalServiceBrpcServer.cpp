#include "GlobalServiceBrpcServer.h"

#include "Logging.h"
#include "OpsMetrics.h"
#include "BrpcSslUtil.h"
#include "PlayerSerialQueue.h"
#include "GameMeshPaths.h"
#include "GlobalServiceForwardServiceImpl.h"

#include <brpc/server.h>
#include <bthread/bthread.h>

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

bool ParseGlobalServiceConfig(const std::string &path, std::string *listen_addr, int *idle_timeout_sec) {
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

const std::string &GlobalServiceCnfPath() {
    static std::string path = "../config/globalservice.cnf";
    static bool once = false;
    if (!once) {
        once = true;
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/globalservice.cnf", &resolved))
            path = std::move(resolved);
    }
    return path;
}

}  // namespace

GlobalServiceBrpcServer &GlobalServiceBrpcServer::Instance() {
    static GlobalServiceBrpcServer g;
    return g;
}

GlobalServiceBrpcServer::~GlobalServiceBrpcServer() { Stop(); }

bool GlobalServiceBrpcServer::StartFromConfig() {
    std::string addr = "0.0.0.0:8301";
    int idle = 30;
    if (!ParseGlobalServiceConfig(GlobalServiceCnfPath(), &addr, &idle))
        LOG_WARN << "GlobalServiceBrpcServer: use default listen, cannot parse " << GlobalServiceCnfPath();
    return Start(addr, idle);
}

bool GlobalServiceBrpcServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;

    PlayerSerialQueue::Instance().Start(0);

    server_.reset(new brpc::Server());
    service_.reset(new GlobalServiceForwardServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GlobalServiceBrpcServer: AddService failed";
        server_.reset();
        service_.reset();
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(GlobalServiceCnfPath(), &ssl);
    if (BrpcSslUtil::ApplyServer(&options, ssl))
        LOG_INFO << "GlobalServiceBrpcServer SSL enabled cert=" << ssl.cert;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "GlobalServiceBrpcServer: Start failed addr=" << listen_addr;
        server_.reset();
        service_.reset();
        return false;
    }
    listen_addr_ = listen_addr;
    OpsMetrics::Instance().SetBrpcWorkerThreads(bthread_getconcurrency());
    running_ = true;
    LOG_INFO << "GlobalServiceBrpcServer listening on " << listen_addr_
             << " (GlobalServiceForward, no combat tick)";
    return true;
}

void GlobalServiceBrpcServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    running_ = false;
    LOG_INFO << "GlobalServiceBrpcServer stopped";
}
